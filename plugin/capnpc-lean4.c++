// Copyright (c) 2026, The Cap'n Proto Authors.
// Licensed under the MIT License:
// https://opensource.org/licenses/MIT

#include <capnp/schema.capnp.h>
#include <capnp/dynamic.h>
#include <capnp/serialize.h>
#include <kj/debug.h>
#include <kj/filesystem.h>
#include <kj/main.h>
#include <kj/string.h>
#include <kj/string-tree.h>
#include <kj/vector.h>

#include <capnp/schema-loader.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef VERSION
#define VERSION "(unknown)"
#endif

namespace capnp {
namespace {

class CapnpcLean4Main {
public:
  CapnpcLean4Main(kj::ProcessContext& context): context(context) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "Cap'n Proto Lean4 plugin version " VERSION,
          "This is a Cap'n Proto compiler plugin which generates Lean4 code. "
          "It is meant to be run using the Cap'n Proto compiler, e.g.:\n"
          "    capnp compile -olean4 foo.capnp")
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  kj::MainBuilder::Validity run() {
    ReaderOptions options;
    options.traversalLimitInWords = 1 << 30;  // Don't limit.
    StreamFdMessageReader reader(0, options);
    auto request = reader.getRoot<schema::CodeGeneratorRequest>();

    for (auto node: request.getNodes()) {
      schemaLoader.load(node);
    }

    buildNameTable(request);

    for (auto requestedFile: request.getRequestedFiles()) {
      auto filename = requestedFile.getFilename();
      auto moduleSegments = pathToModuleSegments(filename);
      auto moduleName = joinSegments(moduleSegments, '.');
      auto outPath = outputPath(moduleSegments);

      auto fileSchema = schemaLoader.get(requestedFile.getId());
      auto fileText = genFile(fileSchema, filename, moduleName);
      writeFile(outPath, kj::strTree(kj::heapString(fileText.c_str())));
    }

    return true;
  }

private:
  using AnnotationList = capnp::List<schema::Annotation, capnp::Kind::STRUCT>::Reader;

  struct QualifiedName {
    kj::String moduleName;
    kj::String typeName;
  };

  kj::ProcessContext& context;
  kj::Own<kj::Filesystem> fs = kj::newDiskFilesystem();
  SchemaLoader schemaLoader;
  std::unordered_map<uint64_t, QualifiedName> nameTable;
  uint64_t gensymCounter = 0;

  static bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  }

  static bool isLeanKeyword(kj::StringPtr ident) {
    static const char* keywords[] = {
      "abbrev", "attribute", "by", "class", "def", "derive", "do", "else", "end",
      "extends", "forall", "fun", "if", "import", "in", "inductive", "instance",
      "let", "match", "mutual", "namespace", "open", "partial", "private", "protected",
      "section", "structure", "theorem", "then", "universe", "variable", "where", "return"
    };
    for (auto kw: keywords) {
      if (ident == kw) return true;
    }
    return false;
  }

  static kj::String sanitizeIdentifier(kj::StringPtr ident) {
    std::string out;
    out.reserve(ident.size());
    for (size_t i = 0; i < ident.size(); ++i) {
      char c = ident[i];
      out.push_back(isIdentChar(c) ? c : '_');
    }
    if (out.empty()) out = "_";
    if (std::isdigit(static_cast<unsigned char>(out[0]))) {
      out.insert(out.begin(), '_');
    }
    if (isLeanKeyword(kj::StringPtr(out.c_str(), out.size()))) {
      out.insert(out.begin(), '_');
    }
    return kj::heapString(out.c_str());
  }

  static kj::String capitalizeIdentifier(kj::StringPtr ident) {
    std::string out(ident.cStr());
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z') {
      out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return kj::heapString(out.c_str());
  }

  static kj::Vector<kj::String> splitPath(kj::StringPtr path) {
    kj::Vector<kj::String> parts;
    size_t start = 0;
    for (size_t i = 0; i < path.size(); ++i) {
      char c = path[i];
      if (c == '/' || c == '\\') {
        if (i > start) {
          parts.add(kj::heapString(path.slice(start, i)));
        }
        start = i + 1;
      }
    }
    if (path.size() > start) {
      parts.add(kj::heapString(path.slice(start, path.size())));
    }
    return parts;
  }

  static kj::Vector<kj::String> splitOnChar(kj::StringPtr text, char sep) {
    kj::Vector<kj::String> parts;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i) {
      if (text[i] == sep) {
        if (i > start) {
          parts.add(kj::heapString(text.slice(start, i)));
        }
        start = i + 1;
      }
    }
    if (text.size() > start) {
      parts.add(kj::heapString(text.slice(start, text.size())));
    }
    return parts;
  }

  static kj::String stripCapnpExtension(kj::StringPtr name) {
    const char* ext = ".capnp";
    size_t extLen = 6;
    if (name.size() >= extLen) {
      bool matches = true;
      for (size_t i = 0; i < extLen; ++i) {
        if (name[name.size() - extLen + i] != ext[i]) {
          matches = false;
          break;
        }
      }
      if (matches) {
        return kj::heapString(name.slice(0, name.size() - extLen));
      }
    }
    return kj::heapString(name);
  }

  static kj::Vector<kj::String> pathToModuleSegments(kj::StringPtr filename) {
    auto parts = splitPath(filename);

    kj::Vector<kj::String> moduleSegments;
    moduleSegments.add(kj::heapString("Capnp"));
    moduleSegments.add(kj::heapString("Gen"));

    if (parts.size() == 0) {
      moduleSegments.add(kj::heapString("Schema"));
      return moduleSegments;
    }

    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      moduleSegments.add(sanitizeIdentifier(parts[i]));
    }

    auto base = stripCapnpExtension(parts[parts.size() - 1]);
    moduleSegments.add(sanitizeIdentifier(base));
    return moduleSegments;
  }

  static kj::String joinSegments(const kj::Vector<kj::String>& segments, char sep) {
    std::string out;
    for (size_t i = 0; i < segments.size(); ++i) {
      if (i > 0) out.push_back(sep);
      out.append(segments[i].cStr());
    }
    return kj::heapString(out.c_str());
  }

  static std::string escapeString(kj::StringPtr text) {
    std::string out;
    out.reserve(text.size() + 2);
    for (size_t i = 0; i < text.size(); ++i) {
      char c = text[i];
      switch (c) {
        case '\\': out += "\\\\"; break;
        case '\"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
          if (static_cast<unsigned char>(c) < 0x20) {
            // Replace other control chars with spaces.
            out += " ";
          } else {
            out.push_back(c);
          }
          break;
      }
    }
    return out;
  }

  static std::string byteArrayLiteral(kj::ArrayPtr<const kj::byte> data) {
    std::string out = "ByteArray.mk #[";
    for (size_t i = 0; i < data.size(); ++i) {
      if (i > 0) out += ", ";
      out += std::to_string(static_cast<uint8_t>(data[i]));
    }
    out += "]";
    return out;
  }

  static kj::String outputPath(const kj::Vector<kj::String>& moduleSegments) {
    std::string out;
    for (size_t i = 0; i < moduleSegments.size(); ++i) {
      if (i > 0) out.push_back('/');
      out.append(moduleSegments[i].cStr());
    }
    out.append(".lean");
    return kj::heapString(out.c_str());
  }

  static bool isPointerType(Type type) {
    switch (type.which()) {
      case schema::Type::TEXT:
      case schema::Type::DATA:
      case schema::Type::STRUCT:
      case schema::Type::LIST:
      case schema::Type::ANY_POINTER:
        return true;
      default:
        return false;
    }
  }

  kj::Maybe<std::string> defaultPointerLiteral(Type type,
                                               schema::Value::Reader value) {
    capnp::MallocMessageBuilder builder;
    auto root = builder.initRoot<capnp::AnyPointer>();

    switch (type.which()) {
      case schema::Type::TEXT:
        if (!value.hasText()) return kj::none;
        root.setAs<capnp::Text>(value.getText());
        break;
      case schema::Type::DATA:
        if (!value.hasData()) return kj::none;
        root.setAs<capnp::Data>(value.getData());
        break;
      case schema::Type::STRUCT:
        if (!value.hasStruct()) return kj::none;
        root.setAs<capnp::DynamicStruct>(
            value.getStruct().getAs<capnp::DynamicStruct>(type.asStruct()));
        break;
      case schema::Type::LIST:
        if (!value.hasList()) return kj::none;
        root.setAs<capnp::DynamicList>(
            value.getList().getAs<capnp::DynamicList>(type.asList()));
        break;
      case schema::Type::ANY_POINTER:
        if (value.hasAnyPointer()) {
          root.setAs<capnp::AnyPointer>(value.getAnyPointer());
          break;
        }
        if (value.hasText()) {
          root.setAs<capnp::Text>(value.getText());
          break;
        }
        if (value.hasData()) {
          root.setAs<capnp::Data>(value.getData());
          break;
        }
        if (value.hasStruct()) {
          root.setAs<capnp::AnyPointer>(value.getStruct());
          break;
        }
        if (value.hasList()) {
          root.setAs<capnp::AnyPointer>(value.getList());
          break;
        }
        return kj::none;
      default:
        return kj::none;
    }

    auto flat = capnp::messageToFlatArray(builder);
    return byteArrayLiteral(flat.asBytes());
  }

  struct DisplayNameParts {
    kj::String filePath;
    kj::String typePath;
  };

  static DisplayNameParts splitDisplayName(kj::StringPtr displayName) {
    for (size_t i = displayName.size(); i > 0; --i) {
      size_t idx = i - 1;
      if (displayName[idx] == ':') {
        return {
          kj::heapString(displayName.slice(0, idx)),
          kj::heapString(displayName.slice(idx + 1, displayName.size()))
        };
      }
    }
    return { kj::heapString(displayName), kj::heapString("") };
  }

  void buildNameTable(schema::CodeGeneratorRequest::Reader request) {
    for (auto node: request.getNodes()) {
      auto schema = schemaLoader.get(node.getId());
      auto displayName = schema.getProto().getDisplayName();
      auto parts = splitDisplayName(displayName);
      auto moduleSegments = pathToModuleSegments(parts.filePath);
      auto moduleName = joinSegments(moduleSegments, '.');
      bool isGroup = false;
      if (node.which() == schema::Node::STRUCT) {
        isGroup = node.getStruct().getIsGroup();
      }

      kj::String typeName = kj::heapString("");
      if (parts.typePath.size() > 0) {
        auto typeParts = splitOnChar(parts.typePath, '.');
        kj::Vector<kj::String> sanitized;
        for (auto& part: typeParts) {
          sanitized.add(sanitizeIdentifier(part));
        }
        if (isGroup && sanitized.size() > 0) {
          auto last = capitalizeIdentifier(sanitized[sanitized.size() - 1]);
          auto groupName = kj::str(last, "Group");
          sanitized[sanitized.size() - 1] = kj::heapString(groupName);
        }
        typeName = joinSegments(sanitized, '.');
      }

      nameTable[node.getId()] = { kj::mv(moduleName), kj::mv(typeName) };
    }
  }

  kj::StringPtr getUnqualifiedName(Schema schema) {
    auto proto = schema.getProto();
    auto parent = schemaLoader.get(proto.getScopeId());
    for (auto nested: parent.getProto().getNestedNodes()) {
      if (nested.getId() == proto.getId()) {
        return nested.getName();
      }
    }
    return proto.getDisplayName();
  }

  kj::String qualifiedTypeName(uint64_t typeId, kj::StringPtr currentModule) {
    auto it = nameTable.find(typeId);
    if (it == nameTable.end() || it->second.typeName.size() == 0) {
      return kj::heapString("Capnp.Unknown");
    }

    if (it->second.moduleName == currentModule) {
      return kj::heapString(it->second.typeName);
    }

    return kj::str(it->second.moduleName, ".", it->second.typeName);
  }

  void collectImportsForType(Type type,
                             kj::StringPtr currentModule,
                             std::set<std::string>& imports) {
    switch (type.which()) {
      case schema::Type::STRUCT: {
        addImportForTypeId(type.asStruct().getProto().getId(), currentModule, imports);
        break;
      }
      case schema::Type::ENUM: {
        addImportForTypeId(type.asEnum().getProto().getId(), currentModule, imports);
        break;
      }
      case schema::Type::INTERFACE: {
        addImportForTypeId(type.asInterface().getProto().getId(), currentModule, imports);
        break;
      }
      case schema::Type::LIST:
        collectImportsForType(type.asList().getElementType(), currentModule, imports);
        break;
      default:
        break;
    }
  }

  void collectImportsForTypeProto(schema::Type::Reader type,
                                  kj::StringPtr currentModule,
                                  std::set<std::string>& imports) {
    switch (type.which()) {
      case schema::Type::STRUCT:
        addImportForTypeId(type.getStruct().getTypeId(), currentModule, imports);
        break;
      case schema::Type::ENUM:
        addImportForTypeId(type.getEnum().getTypeId(), currentModule, imports);
        break;
      case schema::Type::INTERFACE:
        addImportForTypeId(type.getInterface().getTypeId(), currentModule, imports);
        break;
      case schema::Type::LIST:
        collectImportsForTypeProto(type.getList().getElementType(), currentModule, imports);
        break;
      default:
        break;
    }
  }

  void collectImportsForAnnotationList(AnnotationList annotations,
                                       kj::StringPtr currentModule,
                                       std::set<std::string>& imports) {
    for (auto ann: annotations) {
      auto annSchema = schemaLoader.get(ann.getId());
      collectImportsForTypeProto(annSchema.getProto().getAnnotation().getType(),
                                 currentModule, imports);
    }
  }

  void collectImportsForNode(Schema schema,
                             kj::StringPtr currentModule,
                             std::set<std::string>& imports) {
    auto proto = schema.getProto();
    collectImportsForAnnotationList(proto.getAnnotations(), currentModule, imports);
    switch (proto.which()) {
      case schema::Node::STRUCT: {
        auto structSchema = schema.asStruct();
        for (auto field: structSchema.getFields()) {
          collectImportsForType(field.getType(), currentModule, imports);
          collectImportsForAnnotationList(field.getProto().getAnnotations(),
                                          currentModule, imports);
        }
        break;
      }
      case schema::Node::CONST: {
        auto constSchema = schema.asConst();
        collectImportsForType(constSchema.getType(), currentModule, imports);
        break;
      }
      case schema::Node::ANNOTATION: {
        collectImportsForTypeProto(proto.getAnnotation().getType(), currentModule, imports);
        break;
      }
      case schema::Node::ENUM: {
        auto enumSchema = schema.asEnum();
        for (auto enumerant: enumSchema.getEnumerants()) {
          collectImportsForAnnotationList(enumerant.getProto().getAnnotations(),
                                          currentModule, imports);
        }
        break;
      }
      case schema::Node::INTERFACE: {
        auto interfaceSchema = schema.asInterface();
        for (auto method: interfaceSchema.getMethods()) {
          auto paramType = method.getParamType();
          auto resultType = method.getResultType();
          addImportForTypeId(paramType.getProto().getId(), currentModule, imports);
          addImportForTypeId(resultType.getProto().getId(), currentModule, imports);
          collectImportsForAnnotationList(method.getProto().getAnnotations(),
                                          currentModule, imports);
        }
        break;
      }
      default:
        break;
    }

    for (auto nested: proto.getNestedNodes()) {
      collectImportsForNode(schemaLoader.get(nested.getId()), currentModule, imports);
    }
  }

  void addImportForTypeId(uint64_t typeId,
                          kj::StringPtr currentModule,
                          std::set<std::string>& imports) {
    auto it = nameTable.find(typeId);
    if (it == nameTable.end()) return;
    if (it->second.moduleName == currentModule) return;
    imports.insert(it->second.moduleName.cStr());
  }

  std::string typeToLean(Type type, kj::StringPtr currentModule) {
    switch (type.which()) {
      case schema::Type::VOID: return "Unit";
      case schema::Type::BOOL: return "Bool";
      case schema::Type::INT8: return "Int8";
      case schema::Type::INT16: return "Int16";
      case schema::Type::INT32: return "Int32";
      case schema::Type::INT64: return "Int64";
      case schema::Type::UINT8: return "UInt8";
      case schema::Type::UINT16: return "UInt16";
      case schema::Type::UINT32: return "UInt32";
      case schema::Type::UINT64: return "UInt64";
      case schema::Type::FLOAT32: return "Float";
      case schema::Type::FLOAT64: return "Float";
      case schema::Type::TEXT: return "Capnp.Text";
      case schema::Type::DATA: return "Capnp.Data";
      case schema::Type::ANY_POINTER: return "Capnp.AnyPointer";
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
        return name.cStr();
      }
      case schema::Type::ENUM: {
        auto name = qualifiedTypeName(type.asEnum().getProto().getId(), currentModule);
        return name.cStr();
      }
      case schema::Type::INTERFACE: {
        auto name = qualifiedTypeName(type.asInterface().getProto().getId(), currentModule);
        return name.cStr();
      }
      case schema::Type::LIST: {
        auto elem = typeToLean(type.asList().getElementType(), currentModule);
        if (elem.find(' ') != std::string::npos) {
          return "Array (" + elem + ")";
        }
        return "Array " + elem;
      }
      default:
        return "Capnp.Unknown";
    }
  }

  std::string typeToLeanReader(Type type, kj::StringPtr currentModule) {
    switch (type.which()) {
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
        return std::string(name.cStr()) + ".Reader";
      }
      case schema::Type::LIST: {
        auto elem = typeToLeanReader(type.asList().getElementType(), currentModule);
        if (elem.find(' ') != std::string::npos) {
          return "Capnp.ListReader (" + elem + ")";
        }
        return "Capnp.ListReader " + elem;
      }
      default:
        return typeToLean(type, currentModule);
    }
  }

  bool typeSupportsBeq(Type type) {
    switch (type.which()) {
      case schema::Type::LIST:
        return typeSupportsBeq(type.asList().getElementType());
      case schema::Type::VOID:
      case schema::Type::BOOL:
      case schema::Type::INT8:
      case schema::Type::INT16:
      case schema::Type::INT32:
      case schema::Type::INT64:
      case schema::Type::UINT8:
      case schema::Type::UINT16:
      case schema::Type::UINT32:
      case schema::Type::UINT64:
      case schema::Type::FLOAT32:
      case schema::Type::FLOAT64:
      case schema::Type::TEXT:
      case schema::Type::DATA:
      case schema::Type::ANY_POINTER:
      case schema::Type::STRUCT:
      case schema::Type::ENUM:
      case schema::Type::INTERFACE:
        return true;
      default:
        return true;
    }
  }

  std::string readerToValueExpr(Type type,
                                const std::string& readerExpr,
                                kj::StringPtr currentModule) {
    switch (type.which()) {
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
        return std::string(name.cStr()) + ".ofReader " + readerExpr;
      }
      case schema::Type::LIST: {
        auto inner = readerToValueExpr(type.asList().getElementType(), "x", currentModule);
        if (inner == "x") return "Capnp.ListReader.toArray (" + readerExpr + ")";
        return "Capnp.ListReader.toArray (Capnp.ListReader.map (fun x => " +
            inner + ") (" + readerExpr + "))";
      }
      default:
        return readerExpr;
    }
  }

  bool listContainsStruct(Type elemType) {
    switch (elemType.which()) {
      case schema::Type::STRUCT:
        return true;
      case schema::Type::LIST:
        return listContainsStruct(elemType.asList().getElementType());
      default:
        return false;
    }
  }

  std::string listSetterElemType(Type elemType,
                                 kj::StringPtr currentModule,
                                 bool useReaderStructs) {
    if (!useReaderStructs) {
      return typeToLean(elemType, currentModule);
    }

    switch (elemType.which()) {
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(elemType.asStruct().getProto().getId(), currentModule);
        return std::string(name.cStr()) + ".Reader";
      }
      case schema::Type::LIST: {
        auto inner = listSetterElemType(elemType.asList().getElementType(),
                                        currentModule, true);
        if (inner.find(' ') != std::string::npos) {
          return "Capnp.ListReader (" + inner + ")";
        }
        return "Capnp.ListReader " + inner;
      }
      default:
        return typeToLean(elemType, currentModule);
    }
  }

  std::string typeToLeanProto(schema::Type::Reader type, kj::StringPtr currentModule) {
    switch (type.which()) {
      case schema::Type::VOID: return "Unit";
      case schema::Type::BOOL: return "Bool";
      case schema::Type::INT8: return "Int8";
      case schema::Type::INT16: return "Int16";
      case schema::Type::INT32: return "Int32";
      case schema::Type::INT64: return "Int64";
      case schema::Type::UINT8: return "UInt8";
      case schema::Type::UINT16: return "UInt16";
      case schema::Type::UINT32: return "UInt32";
      case schema::Type::UINT64: return "UInt64";
      case schema::Type::FLOAT32: return "Float";
      case schema::Type::FLOAT64: return "Float";
      case schema::Type::TEXT: return "Capnp.Text";
      case schema::Type::DATA: return "Capnp.Data";
      case schema::Type::ANY_POINTER: return "Capnp.AnyPointer";
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(type.getStruct().getTypeId(), currentModule);
        return name.cStr();
      }
      case schema::Type::ENUM: {
        auto name = qualifiedTypeName(type.getEnum().getTypeId(), currentModule);
        return name.cStr();
      }
      case schema::Type::INTERFACE: {
        auto name = qualifiedTypeName(type.getInterface().getTypeId(), currentModule);
        return name.cStr();
      }
      case schema::Type::LIST: {
        auto elem = typeToLeanProto(type.getList().getElementType(), currentModule);
        if (elem.find(' ') != std::string::npos) {
          return "Array (" + elem + ")";
        }
        return "Array " + elem;
      }
      default:
        return "Capnp.Unknown";
    }
  }

  Type protoTypeToType(schema::Type::Reader type) {
    switch (type.which()) {
      case schema::Type::VOID: return Type(schema::Type::VOID);
      case schema::Type::BOOL: return Type(schema::Type::BOOL);
      case schema::Type::INT8: return Type(schema::Type::INT8);
      case schema::Type::INT16: return Type(schema::Type::INT16);
      case schema::Type::INT32: return Type(schema::Type::INT32);
      case schema::Type::INT64: return Type(schema::Type::INT64);
      case schema::Type::UINT8: return Type(schema::Type::UINT8);
      case schema::Type::UINT16: return Type(schema::Type::UINT16);
      case schema::Type::UINT32: return Type(schema::Type::UINT32);
      case schema::Type::UINT64: return Type(schema::Type::UINT64);
      case schema::Type::FLOAT32: return Type(schema::Type::FLOAT32);
      case schema::Type::FLOAT64: return Type(schema::Type::FLOAT64);
      case schema::Type::TEXT: return Type(schema::Type::TEXT);
      case schema::Type::DATA: return Type(schema::Type::DATA);
      case schema::Type::ANY_POINTER: return Type(schema::Type::ANY_POINTER);
      case schema::Type::STRUCT: {
        auto schema = schemaLoader.get(type.getStruct().getTypeId()).asStruct();
        return Type(schema);
      }
      case schema::Type::ENUM: {
        auto schema = schemaLoader.get(type.getEnum().getTypeId()).asEnum();
        return Type(schema);
      }
      case schema::Type::INTERFACE: {
        auto schema = schemaLoader.get(type.getInterface().getTypeId()).asInterface();
        return Type(schema);
      }
      case schema::Type::LIST: {
        auto elem = protoTypeToType(type.getList().getElementType());
        return Type(ListSchema::of(elem));
      }
      default:
        return Type(schema::Type::VOID);
    }
  }

  std::string valueToLeanProto(schema::Type::Reader type,
                               schema::Value::Reader value,
                               kj::StringPtr currentModule) {
    auto t = protoTypeToType(type);
    return valueToLean(t, value, currentModule);
  }

  std::string listReadExpr(Type elemType,
                           const std::string& ptrExpr,
                           kj::StringPtr currentModule) {
    switch (elemType.which()) {
      case schema::Type::VOID:
        return "Capnp.readListVoidReader " + ptrExpr;
      case schema::Type::BOOL:
        return "Capnp.readListBoolReader " + ptrExpr;
      case schema::Type::INT8:
        return "Capnp.readListInt8Reader " + ptrExpr;
      case schema::Type::INT16:
        return "Capnp.readListInt16Reader " + ptrExpr;
      case schema::Type::INT32:
        return "Capnp.readListInt32Reader " + ptrExpr;
      case schema::Type::INT64:
        return "Capnp.readListInt64Reader " + ptrExpr;
      case schema::Type::UINT8:
        return "Capnp.readListUInt8Reader " + ptrExpr;
      case schema::Type::UINT16:
        return "Capnp.readListUInt16Reader " + ptrExpr;
      case schema::Type::UINT32:
        return "Capnp.readListUInt32Reader " + ptrExpr;
      case schema::Type::UINT64:
        return "Capnp.readListUInt64Reader " + ptrExpr;
      case schema::Type::FLOAT32:
        return "Capnp.readListFloat32Reader " + ptrExpr;
      case schema::Type::FLOAT64:
        return "Capnp.readListFloat64Reader " + ptrExpr;
      case schema::Type::TEXT:
        return "Capnp.readListTextReader " + ptrExpr;
      case schema::Type::DATA:
        return "Capnp.readListDataReader " + ptrExpr;
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(elemType.asStruct().getProto().getId(), currentModule);
        return "Capnp.ListReader.map " + std::string(name.cStr()) +
            ".fromStruct (Capnp.readListStructReader " + ptrExpr + ")";
      }
      case schema::Type::ENUM: {
        auto name = qualifiedTypeName(elemType.asEnum().getProto().getId(), currentModule);
        return "Capnp.ListReader.map " + std::string(name.cStr()) +
            ".ofUInt16 (Capnp.readListUInt16Reader " + ptrExpr + ")";
      }
      case schema::Type::INTERFACE:
        return "Capnp.readListCapabilityReader " + ptrExpr;
      case schema::Type::ANY_POINTER:
        return "Capnp.readListPointerReader " + ptrExpr;
      case schema::Type::LIST: {
        auto inner = listReadExpr(elemType.asList().getElementType(), "p", currentModule);
        return "Capnp.ListReader.map (fun p => " + inner + ") (Capnp.readListPointerReader " +
            ptrExpr + ")";
      }
      default:
        return "Capnp.readListPointerReader " + ptrExpr;
    }
  }

  std::string listReadExprChecked(Type elemType,
                                  const std::string& ptrExpr,
                                  kj::StringPtr currentModule) {
    switch (elemType.which()) {
      case schema::Type::VOID:
        return "Capnp.readListVoidCheckedReader " + ptrExpr;
      case schema::Type::BOOL:
        return "Capnp.readListBoolCheckedReader " + ptrExpr;
      case schema::Type::INT8:
        return "Capnp.readListInt8CheckedReader " + ptrExpr;
      case schema::Type::INT16:
        return "Capnp.readListInt16CheckedReader " + ptrExpr;
      case schema::Type::INT32:
        return "Capnp.readListInt32CheckedReader " + ptrExpr;
      case schema::Type::INT64:
        return "Capnp.readListInt64CheckedReader " + ptrExpr;
      case schema::Type::UINT8:
        return "Capnp.readListUInt8CheckedReader " + ptrExpr;
      case schema::Type::UINT16:
        return "Capnp.readListUInt16CheckedReader " + ptrExpr;
      case schema::Type::UINT32:
        return "Capnp.readListUInt32CheckedReader " + ptrExpr;
      case schema::Type::UINT64:
        return "Capnp.readListUInt64CheckedReader " + ptrExpr;
      case schema::Type::FLOAT32:
        return "Capnp.readListFloat32CheckedReader " + ptrExpr;
      case schema::Type::FLOAT64:
        return "Capnp.readListFloat64CheckedReader " + ptrExpr;
      case schema::Type::TEXT:
        return "do\n  let r ← Capnp.readListPointerCheckedReader " + ptrExpr +
            "\n  return Capnp.ListReader.map Capnp.readText r";
      case schema::Type::DATA:
        return "do\n  let r ← Capnp.readListPointerCheckedReader " + ptrExpr +
            "\n  return Capnp.ListReader.map Capnp.readData r";
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(elemType.asStruct().getProto().getId(), currentModule);
        return "do\n  let r ← Capnp.readListStructCheckedReader " + ptrExpr +
            "\n  return Capnp.ListReader.map " + std::string(name.cStr()) + ".fromStruct r";
      }
      case schema::Type::ENUM: {
        auto name = qualifiedTypeName(elemType.asEnum().getProto().getId(), currentModule);
        return "do\n  let r ← Capnp.readListUInt16CheckedReader " + ptrExpr +
            "\n  return Capnp.ListReader.map " + std::string(name.cStr()) + ".ofUInt16 r";
      }
      case schema::Type::INTERFACE:
        return "do\n  let r ← Capnp.readListPointerCheckedReader " + ptrExpr +
            "\n  return Capnp.ListReader.map Capnp.readCapability r";
      case schema::Type::ANY_POINTER:
        return "Capnp.readListPointerCheckedReader " + ptrExpr;
      case schema::Type::LIST: {
        auto inner = listReadExpr(elemType.asList().getElementType(), "p", currentModule);
        return "do\n  let r ← Capnp.readListPointerCheckedReader " + ptrExpr +
            "\n  return Capnp.ListReader.map (fun p => " + inner + ") r";
      }
      default:
        return "Capnp.readListPointerCheckedReader " + ptrExpr;
    }
  }

  void emitListWrite(Type elemType,
                     const std::string& ptrExpr,
                     const std::string& valsExpr,
                     kj::StringPtr currentModule,
                     std::string& out,
                     const std::string& indent,
                     bool fromReaderValues = false) {
    auto ptr = "(" + ptrExpr + ")";
    auto vals = "(" + valsExpr + ")";
    auto valsSize = vals + ".size";
    switch (elemType.which()) {
      case schema::Type::VOID:
        out += indent + "Capnp.writeListVoid " + ptr + " " + vals + "\n";
        break;
      case schema::Type::BOOL:
        out += indent + "Capnp.writeListBool " + ptr + " " + vals + "\n";
        break;
      case schema::Type::INT8:
        out += indent + "Capnp.writeListInt8 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::INT16:
        out += indent + "Capnp.writeListInt16 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::INT32:
        out += indent + "Capnp.writeListInt32 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::INT64:
        out += indent + "Capnp.writeListInt64 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::UINT8:
        out += indent + "Capnp.writeListUInt8 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::UINT16:
        out += indent + "Capnp.writeListUInt16 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::UINT32:
        out += indent + "Capnp.writeListUInt32 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::UINT64:
        out += indent + "Capnp.writeListUInt64 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::FLOAT32:
        out += indent + "Capnp.writeListFloat32 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::FLOAT64:
        out += indent + "Capnp.writeListFloat64 " + ptr + " " + vals + "\n";
        break;
      case schema::Type::TEXT:
        out += indent + "Capnp.writeListText " + ptr + " " + vals + "\n";
        break;
      case schema::Type::DATA:
        out += indent + "Capnp.writeListData " + ptr + " " + vals + "\n";
        break;
      case schema::Type::ENUM: {
        auto name = qualifiedTypeName(elemType.asEnum().getProto().getId(), currentModule);
        out += indent + "Capnp.writeListUInt16 " + ptr + " (Array.map " +
            std::string(name.cStr()) + ".toUInt16 " + vals + ")\n";
        break;
      }
      case schema::Type::STRUCT: {
        auto elemName = qualifiedTypeName(elemType.asStruct().getProto().getId(), currentModule);
        auto elemSchema = elemType.asStruct();
        auto elemData = elemSchema.getProto().getStruct().getDataWordCount();
        auto elemPtrs = elemSchema.getProto().getStruct().getPointerCount();
        auto idxVar = freshName("i");
        auto elemVar = freshName("elem");
        out += indent + "let lb ← Capnp.initStructListPointer " + ptr + " " +
            std::to_string(elemData) + " " + std::to_string(elemPtrs) + " " + valsSize + "\n";
        out += indent + "let builders := Capnp.listStructBuilders lb\n";
        out += indent + "let emptySb : Capnp.StructBuilder :=\n";
        out += indent + "  { seg := 0, dataOff := 0, dataWords := 0, ptrOff := 0, ptrCount := 0 }\n";
        out += indent + "let mut " + idxVar + " := 0\n";
        out += indent + "for " + elemVar + " in " + vals + " do\n";
        out += indent + "  let sb := builders.getD " + idxVar + " emptySb\n";
        if (fromReaderValues) {
          out += indent + "  Capnp.copyStruct sb " + elemVar + ".struct\n";
        } else {
          out += indent + "  " + std::string(elemName.cStr()) +
              ".Builder.setFromValue (" + std::string(elemName.cStr()) + ".Builder.fromStruct sb) " + elemVar + "\n";
        }
        out += indent + "  " + idxVar + " := " + idxVar + " + 1\n";
        break;
      }
      case schema::Type::INTERFACE:
        out += indent + "Capnp.writeListCapability " + ptr + " " + vals + "\n";
        break;
      case schema::Type::ANY_POINTER:
      {
        auto idxVar = freshName("i");
        auto elemVar = freshName("elem");
        out += indent + "let ptrs ← Capnp.writeListPointer " + ptr + " " +
            valsSize + "\n";
        out += indent + "let mut " + idxVar + " := 0\n";
        out += indent + "for " + elemVar + " in " + vals + " do\n";
        out += indent + "  let p := ptrs.getD " + idxVar + " { seg := 0, word := 0 }\n";
        out += indent + "  Capnp.copyAnyPointer p " + elemVar + "\n";
        out += indent + "  " + idxVar + " := " + idxVar + " + 1\n";
        break;
      }
      case schema::Type::LIST: {
        auto idxVar = freshName("i");
        auto elemVar = freshName("elem");
        out += indent + "let ptrs ← Capnp.writeListPointer " + ptr + " " +
            valsSize + "\n";
        out += indent + "let mut " + idxVar + " := 0\n";
        out += indent + "for " + elemVar + " in " + vals + " do\n";
        out += indent + "  let p := ptrs.getD " + idxVar + " { seg := 0, word := 0 }\n";
        auto nestedVals = fromReaderValues
            ? "Capnp.ListReader.toArray (" + elemVar + ")"
            : elemVar;
        emitListWrite(elemType.asList().getElementType(), "p", nestedVals, currentModule,
                      out, indent + "  ", fromReaderValues);
        out += indent + "  " + idxVar + " := " + idxVar + " + 1\n";
        break;
      }
      default:
        out += indent + "Capnp.writeListPointer " + ptr + " " + valsSize + "\n";
        break;
    }
  }

  std::string valueToLean(Type type,
                          schema::Value::Reader value,
                          kj::StringPtr currentModule) {
    switch (type.which()) {
      case schema::Type::VOID:
        return "()";
      case schema::Type::BOOL:
        return value.getBool() ? "true" : "false";
      case schema::Type::INT8:
        return "Int8.ofInt (" + std::to_string(value.getInt8()) + ")";
      case schema::Type::INT16:
        return "Int16.ofInt (" + std::to_string(value.getInt16()) + ")";
      case schema::Type::INT32:
        return "Int32.ofInt (" + std::to_string(value.getInt32()) + ")";
      case schema::Type::INT64:
        return "Int64.ofInt (" + std::to_string(value.getInt64()) + ")";
      case schema::Type::UINT8:
        return "UInt8.ofNat " + std::to_string(value.getUint8());
      case schema::Type::UINT16:
        return "UInt16.ofNat " + std::to_string(value.getUint16());
      case schema::Type::UINT32:
        return "UInt32.ofNat " + std::to_string(value.getUint32());
      case schema::Type::UINT64:
        return "UInt64.ofNat " + std::to_string(value.getUint64());
      case schema::Type::FLOAT32:
        return kj::str(value.getFloat32()).cStr();
      case schema::Type::FLOAT64:
        return kj::str(value.getFloat64()).cStr();
      case schema::Type::TEXT: {
        auto text = value.getText();
        return "\"" + escapeString(text) + "\"";
      }
      case schema::Type::DATA: {
        auto data = value.getData();
        return byteArrayLiteral(data);
      }
      case schema::Type::STRUCT: {
        auto literal = defaultPointerLiteral(type, value);
        if (literal == kj::none) return "";
        auto name = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
        std::string ptrExpr = "Capnp.getRoot (Capnp.readMessage (" +
            KJ_ASSERT_NONNULL(literal) + "))";
        return std::string(name.cStr()) + ".ofReader (" + std::string(name.cStr()) +
            ".read (" + ptrExpr + "))";
      }
      case schema::Type::LIST: {
        auto literal = defaultPointerLiteral(type, value);
        if (literal == kj::none) return "";
        std::string ptrExpr = "(Capnp.getRoot (Capnp.readMessage (" +
            KJ_ASSERT_NONNULL(literal) + ")))";
        auto readExpr = listReadExpr(type.asList().getElementType(), ptrExpr, currentModule);
        return readerToValueExpr(type, readExpr, currentModule);
      }
      case schema::Type::INTERFACE:
        return "0";
      case schema::Type::ANY_POINTER: {
        auto literal = defaultPointerLiteral(type, value);
        if (literal == kj::none) return "";
        return "Capnp.getRoot (Capnp.readMessage (" + KJ_ASSERT_NONNULL(literal) + "))";
      }
      case schema::Type::ENUM: {
        auto name = qualifiedTypeName(type.asEnum().getProto().getId(), currentModule);
        return std::string(name.cStr()) + ".ofUInt16 " + std::to_string(value.getEnum());
      }
      default:
        return "";
    }
  }

  std::string uniqueName(const std::string& base, std::set<std::string>& used) {
    std::string candidate = base;
    int counter = 1;
    while (used.count(candidate) != 0) {
      candidate = base + "_" + std::to_string(counter++);
    }
    used.insert(candidate);
    return candidate;
  }

  std::string freshName(const std::string& base) {
    return base + "_" + std::to_string(gensymCounter++);
  }

  struct UnionInfo {
    std::string noneName;
    std::vector<std::pair<StructSchema::Field, std::string>> cases;
  };

  UnionInfo buildUnionInfo(StructSchema structSchema) {
    UnionInfo info;
    auto unionFields = structSchema.getUnionFields();
    if (unionFields.size() == 0) return info;

    std::string noneName = "none";
    bool noneConflict = false;
    for (auto field: unionFields) {
      auto rawName = sanitizeIdentifier(field.getProto().getName());
      if (rawName == "none") {
        noneConflict = true;
        break;
      }
    }
    if (noneConflict) noneName = "none_";
    info.noneName = noneName;

    std::set<std::string> usedUnion;
    usedUnion.insert(noneName);
    for (auto field: unionFields) {
      auto rawName = sanitizeIdentifier(field.getProto().getName());
      auto ctor = uniqueName(rawName.cStr(), usedUnion);
      info.cases.push_back({field, ctor});
    }

    return info;
  }

  std::string genFieldAccessor(StructSchema::Field field,
                               kj::StringPtr structName,
                               kj::StringPtr currentModule,
                               const std::string& accessorName,
                               const std::unordered_map<uint, std::string>& defaultPointers) {
    auto readerVar = std::string("r");
    if (!field.getProto().isGroup() && field.getType().which() == schema::Type::VOID) {
      readerVar = "_r";
    }

    std::string out;
    out += "def ";
    out += structName.cStr();
    out += ".Reader.";
    out += accessorName;
    out += " (";
    out += readerVar;
    out += " : ";
    out += structName.cStr();
    out += ".Reader) : ";
    std::string retType;

    if (field.getProto().isGroup()) {
      auto groupTypeId = field.getProto().getGroup().getTypeId();
      auto groupName = qualifiedTypeName(groupTypeId, currentModule);
      retType = std::string(groupName.cStr()) + ".Reader";
    } else {
      retType = typeToLeanReader(field.getType(), currentModule);
    }

    out += retType;
    out += " := ";

    if (field.getProto().isGroup()) {
      auto groupTypeId = field.getProto().getGroup().getTypeId();
      auto groupName = qualifiedTypeName(groupTypeId, currentModule);
      out += groupName.cStr();
      out += ".fromStruct r.struct";
      return out;
    }

    auto slot = field.getProto().getSlot();
    auto offset = slot.getOffset();
    auto defaultValue = slot.getDefaultValue();
    auto type = field.getType();
    std::string defaultPtrExpr;
    auto defIt = defaultPointers.find(field.getIndex());
    if (defIt != defaultPointers.end()) {
      defaultPtrExpr = defIt->second;
    }

    switch (type.which()) {
      case schema::Type::VOID:
        out += "()";
        break;
      case schema::Type::BOOL:
        if (defaultValue.getBool()) {
          out += "Capnp.getBoolMasked r.struct ";
          out += std::to_string(offset);
          out += " true";
        } else {
          out += "Capnp.getBool r.struct ";
          out += std::to_string(offset);
        }
        break;
      case schema::Type::INT8:
        if (defaultValue.getInt8() != 0) {
          uint8_t mask = static_cast<uint8_t>(defaultValue.getInt8());
          out += "Capnp.getInt8Masked r.struct ";
          out += std::to_string(offset);
          out += " (UInt8.ofNat ";
          out += std::to_string(mask);
          out += ")";
        } else {
          out += "Capnp.getInt8 r.struct ";
          out += std::to_string(offset);
        }
        break;
      case schema::Type::UINT8:
        if (defaultValue.getUint8() != 0) {
          out += "Capnp.getUInt8Masked r.struct ";
          out += std::to_string(offset);
          out += " (UInt8.ofNat ";
          out += std::to_string(defaultValue.getUint8());
          out += ")";
        } else {
          out += "Capnp.getUInt8 r.struct ";
          out += std::to_string(offset);
        }
        break;
      case schema::Type::INT16:
        if (defaultValue.getInt16() != 0) {
          uint16_t mask = static_cast<uint16_t>(defaultValue.getInt16());
          out += "Capnp.getInt16Masked r.struct ";
          out += std::to_string(offset * 2);
          out += " (UInt16.ofNat ";
          out += std::to_string(mask);
          out += ")";
        } else {
          out += "Capnp.getInt16 r.struct ";
          out += std::to_string(offset * 2);
        }
        break;
      case schema::Type::UINT16:
        if (defaultValue.getUint16() != 0) {
          out += "Capnp.getUInt16Masked r.struct ";
          out += std::to_string(offset * 2);
          out += " (UInt16.ofNat ";
          out += std::to_string(defaultValue.getUint16());
          out += ")";
        } else {
          out += "Capnp.getUInt16 r.struct ";
          out += std::to_string(offset * 2);
        }
        break;
      case schema::Type::INT32:
        if (defaultValue.getInt32() != 0) {
          uint32_t mask = static_cast<uint32_t>(defaultValue.getInt32());
          out += "Capnp.getInt32Masked r.struct ";
          out += std::to_string(offset * 4);
          out += " (UInt32.ofNat ";
          out += std::to_string(mask);
          out += ")";
        } else {
          out += "Capnp.getInt32 r.struct ";
          out += std::to_string(offset * 4);
        }
        break;
      case schema::Type::UINT32:
        if (defaultValue.getUint32() != 0) {
          out += "Capnp.getUInt32Masked r.struct ";
          out += std::to_string(offset * 4);
          out += " (UInt32.ofNat ";
          out += std::to_string(defaultValue.getUint32());
          out += ")";
        } else {
          out += "Capnp.getUInt32 r.struct ";
          out += std::to_string(offset * 4);
        }
        break;
      case schema::Type::INT64:
        if (defaultValue.getInt64() != 0) {
          uint64_t mask = static_cast<uint64_t>(defaultValue.getInt64());
          out += "Capnp.getInt64Masked r.struct ";
          out += std::to_string(offset * 8);
          out += " (UInt64.ofNat ";
          out += std::to_string(mask);
          out += ")";
        } else {
          out += "Capnp.getInt64 r.struct ";
          out += std::to_string(offset * 8);
        }
        break;
      case schema::Type::UINT64:
        if (defaultValue.getUint64() != 0) {
          out += "Capnp.getUInt64Masked r.struct ";
          out += std::to_string(offset * 8);
          out += " (UInt64.ofNat ";
          out += std::to_string(defaultValue.getUint64());
          out += ")";
        } else {
          out += "Capnp.getUInt64 r.struct ";
          out += std::to_string(offset * 8);
        }
        break;
      case schema::Type::FLOAT32:
        if (defaultValue.getFloat32() != 0.0f) {
          uint32_t mask;
          float v = defaultValue.getFloat32();
          memcpy(&mask, &v, sizeof(mask));
          out += "Capnp.getFloat32Masked r.struct ";
          out += std::to_string(offset * 4);
          out += " (UInt32.ofNat ";
          out += std::to_string(mask);
          out += ")";
        } else {
          out += "Capnp.getFloat32 r.struct ";
          out += std::to_string(offset * 4);
        }
        break;
      case schema::Type::FLOAT64:
        if (defaultValue.getFloat64() != 0.0) {
          uint64_t mask;
          double v = defaultValue.getFloat64();
          memcpy(&mask, &v, sizeof(mask));
          out += "Capnp.getFloat64Masked r.struct ";
          out += std::to_string(offset * 8);
          out += " (UInt64.ofNat ";
          out += std::to_string(mask);
          out += ")";
        } else {
          out += "Capnp.getFloat64 r.struct ";
          out += std::to_string(offset * 8);
        }
        break;
      case schema::Type::ENUM: {
        auto enumName = qualifiedTypeName(type.asEnum().getProto().getId(), currentModule);
        out += enumName.cStr();
        if (defaultValue.getEnum() != 0) {
          out += ".ofUInt16 (Capnp.getUInt16Masked r.struct ";
          out += std::to_string(offset * 2);
          out += " (UInt16.ofNat ";
          out += std::to_string(defaultValue.getEnum());
          out += "))";
        } else {
          out += ".ofUInt16 (Capnp.getUInt16 r.struct ";
          out += std::to_string(offset * 2);
          out += ")";
        }
        break;
      }
      case schema::Type::TEXT:
        if (defaultPtrExpr.empty()) {
          out += "Capnp.readText (Capnp.getPointer r.struct ";
          out += std::to_string(offset);
          out += ")";
        } else {
          out += "Capnp.readText (Capnp.withDefaultPointer (Capnp.getPointer r.struct ";
          out += std::to_string(offset);
          out += ") ";
          out += defaultPtrExpr;
          out += ")";
        }
        break;
      case schema::Type::DATA:
        if (defaultPtrExpr.empty()) {
          out += "Capnp.readData (Capnp.getPointer r.struct ";
          out += std::to_string(offset);
          out += ")";
        } else {
          out += "Capnp.readData (Capnp.withDefaultPointer (Capnp.getPointer r.struct ";
          out += std::to_string(offset);
          out += ") ";
          out += defaultPtrExpr;
          out += ")";
        }
        break;
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
        out += name.cStr();
        out += ".read (";
        if (defaultPtrExpr.empty()) {
          out += "Capnp.getPointer r.struct ";
          out += std::to_string(offset);
        } else {
          out += "Capnp.withDefaultPointer (Capnp.getPointer r.struct ";
          out += std::to_string(offset);
          out += ") ";
          out += defaultPtrExpr;
        }
        out += ")";
        break;
      }
      case schema::Type::INTERFACE:
        out += "Capnp.readCapability (Capnp.getPointer r.struct ";
        out += std::to_string(offset);
        out += ")";
        break;
      case schema::Type::ANY_POINTER:
        if (defaultPtrExpr.empty()) {
          out += "Capnp.getPointer r.struct ";
          out += std::to_string(offset);
        } else {
          out += "Capnp.withDefaultPointer (Capnp.getPointer r.struct ";
          out += std::to_string(offset);
          out += ") ";
          out += defaultPtrExpr;
        }
        break;
      case schema::Type::LIST: {
        std::string ptrExpr;
        if (defaultPtrExpr.empty()) {
          ptrExpr = "(Capnp.getPointer r.struct " + std::to_string(offset) + ")";
        } else {
          ptrExpr = "(Capnp.withDefaultPointer (Capnp.getPointer r.struct " +
              std::to_string(offset) + ") " + defaultPtrExpr + ")";
        }
        out += listReadExpr(type.asList().getElementType(), ptrExpr, currentModule);
        break;
      }
    }

    return out;
  }

  std::string genFieldAccessorChecked(StructSchema::Field field,
                                      kj::StringPtr structName,
                                      kj::StringPtr currentModule,
                                      const std::string& accessorName,
                                      const std::unordered_map<uint, std::string>& defaultPointers) {
    auto readerVar = std::string("r");
    if (!field.getProto().isGroup() && field.getType().which() == schema::Type::VOID) {
      readerVar = "_r";
    }

    std::string out;
    out += "def ";
    out += structName.cStr();
    out += ".Reader.";
    out += accessorName;
    out += " (";
    out += readerVar;
    out += " : ";
    out += structName.cStr();
    out += ".Reader) : Except String (";
    std::string retType;

    if (field.getProto().isGroup()) {
      auto groupTypeId = field.getProto().getGroup().getTypeId();
      auto groupName = qualifiedTypeName(groupTypeId, currentModule);
      retType = std::string(groupName.cStr()) + ".Reader";
      out += retType;
      out += ") := pure (";
      out += groupName.cStr();
      out += ".fromStruct r.struct)";
      return out;
    } else {
      retType = typeToLeanReader(field.getType(), currentModule);
    }

    out += retType;
    out += ") := ";

    auto slot = field.getProto().getSlot();
    auto offset = slot.getOffset();
    auto defaultValue = slot.getDefaultValue();
    auto type = field.getType();
    std::string defaultPtrExpr;
    auto defIt = defaultPointers.find(field.getIndex());
    if (defIt != defaultPointers.end()) {
      defaultPtrExpr = defIt->second;
    }

    auto pointerExpr = [&]() -> std::string {
      if (defaultPtrExpr.empty()) {
        return "Capnp.getPointer r.struct " + std::to_string(offset);
      } else {
        return "Capnp.withDefaultPointer (Capnp.getPointer r.struct " +
            std::to_string(offset) + ") " + defaultPtrExpr;
      }
    };

    switch (type.which()) {
      case schema::Type::VOID:
        out += "pure ()";
        break;
      case schema::Type::BOOL:
        if (defaultValue.getBool()) {
          out += "pure (Capnp.getBoolMasked r.struct ";
          out += std::to_string(offset);
          out += " true)";
        } else {
          out += "pure (Capnp.getBool r.struct ";
          out += std::to_string(offset);
          out += ")";
        }
        break;
      case schema::Type::INT8:
        if (defaultValue.getInt8() != 0) {
          uint8_t mask = static_cast<uint8_t>(defaultValue.getInt8());
          out += "pure (Capnp.getInt8Masked r.struct ";
          out += std::to_string(offset);
          out += " (UInt8.ofNat ";
          out += std::to_string(mask);
          out += "))";
        } else {
          out += "pure (Capnp.getInt8 r.struct ";
          out += std::to_string(offset);
          out += ")";
        }
        break;
      case schema::Type::UINT8:
        if (defaultValue.getUint8() != 0) {
          out += "pure (Capnp.getUInt8Masked r.struct ";
          out += std::to_string(offset);
          out += " (UInt8.ofNat ";
          out += std::to_string(defaultValue.getUint8());
          out += "))";
        } else {
          out += "pure (Capnp.getUInt8 r.struct ";
          out += std::to_string(offset);
          out += ")";
        }
        break;
      case schema::Type::INT16:
        if (defaultValue.getInt16() != 0) {
          uint16_t mask = static_cast<uint16_t>(defaultValue.getInt16());
          out += "pure (Capnp.getInt16Masked r.struct ";
          out += std::to_string(offset * 2);
          out += " (UInt16.ofNat ";
          out += std::to_string(mask);
          out += "))";
        } else {
          out += "pure (Capnp.getInt16 r.struct ";
          out += std::to_string(offset * 2);
          out += ")";
        }
        break;
      case schema::Type::UINT16:
        if (defaultValue.getUint16() != 0) {
          out += "pure (Capnp.getUInt16Masked r.struct ";
          out += std::to_string(offset * 2);
          out += " (UInt16.ofNat ";
          out += std::to_string(defaultValue.getUint16());
          out += "))";
        } else {
          out += "pure (Capnp.getUInt16 r.struct ";
          out += std::to_string(offset * 2);
          out += ")";
        }
        break;
      case schema::Type::INT32:
        if (defaultValue.getInt32() != 0) {
          uint32_t mask = static_cast<uint32_t>(defaultValue.getInt32());
          out += "pure (Capnp.getInt32Masked r.struct ";
          out += std::to_string(offset * 4);
          out += " (UInt32.ofNat ";
          out += std::to_string(mask);
          out += "))";
        } else {
          out += "pure (Capnp.getInt32 r.struct ";
          out += std::to_string(offset * 4);
          out += ")";
        }
        break;
      case schema::Type::UINT32:
        if (defaultValue.getUint32() != 0) {
          out += "pure (Capnp.getUInt32Masked r.struct ";
          out += std::to_string(offset * 4);
          out += " (UInt32.ofNat ";
          out += std::to_string(defaultValue.getUint32());
          out += "))";
        } else {
          out += "pure (Capnp.getUInt32 r.struct ";
          out += std::to_string(offset * 4);
          out += ")";
        }
        break;
      case schema::Type::INT64:
        if (defaultValue.getInt64() != 0) {
          uint64_t mask = static_cast<uint64_t>(defaultValue.getInt64());
          out += "pure (Capnp.getInt64Masked r.struct ";
          out += std::to_string(offset * 8);
          out += " (UInt64.ofNat ";
          out += std::to_string(mask);
          out += "))";
        } else {
          out += "pure (Capnp.getInt64 r.struct ";
          out += std::to_string(offset * 8);
          out += ")";
        }
        break;
      case schema::Type::UINT64:
        if (defaultValue.getUint64() != 0) {
          out += "pure (Capnp.getUInt64Masked r.struct ";
          out += std::to_string(offset * 8);
          out += " (UInt64.ofNat ";
          out += std::to_string(defaultValue.getUint64());
          out += "))";
        } else {
          out += "pure (Capnp.getUInt64 r.struct ";
          out += std::to_string(offset * 8);
          out += ")";
        }
        break;
      case schema::Type::FLOAT32:
        if (defaultValue.getFloat32() != 0.0f) {
          uint32_t mask;
          float v = defaultValue.getFloat32();
          memcpy(&mask, &v, sizeof(mask));
          out += "pure (Capnp.getFloat32Masked r.struct ";
          out += std::to_string(offset * 4);
          out += " (UInt32.ofNat ";
          out += std::to_string(mask);
          out += "))";
        } else {
          out += "pure (Capnp.getFloat32 r.struct ";
          out += std::to_string(offset * 4);
          out += ")";
        }
        break;
      case schema::Type::FLOAT64:
        if (defaultValue.getFloat64() != 0.0) {
          uint64_t mask;
          double v = defaultValue.getFloat64();
          memcpy(&mask, &v, sizeof(mask));
          out += "pure (Capnp.getFloat64Masked r.struct ";
          out += std::to_string(offset * 8);
          out += " (UInt64.ofNat ";
          out += std::to_string(mask);
          out += "))";
        } else {
          out += "pure (Capnp.getFloat64 r.struct ";
          out += std::to_string(offset * 8);
          out += ")";
        }
        break;
      case schema::Type::ENUM: {
        auto enumName = qualifiedTypeName(type.asEnum().getProto().getId(), currentModule);
        if (defaultValue.getEnum() != 0) {
          out += "pure (";
          out += enumName.cStr();
          out += ".ofUInt16 (Capnp.getUInt16Masked r.struct ";
          out += std::to_string(offset * 2);
          out += " (UInt16.ofNat ";
          out += std::to_string(defaultValue.getEnum());
          out += ")))";
        } else {
          out += "pure (";
          out += enumName.cStr();
          out += ".ofUInt16 (Capnp.getUInt16 r.struct ";
          out += std::to_string(offset * 2);
          out += "))";
        }
        break;
      }
      case schema::Type::TEXT:
        out += "Capnp.readTextChecked (" + pointerExpr() + ")";
        break;
      case schema::Type::DATA:
        out += "Capnp.readDataChecked (" + pointerExpr() + ")";
        break;
      case schema::Type::STRUCT: {
        auto name = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
        out += std::string(name.cStr()) + ".readChecked (" + pointerExpr() + ")";
        break;
      }
      case schema::Type::INTERFACE:
        out += "Capnp.readCapabilityChecked (" + pointerExpr() + ")";
        break;
      case schema::Type::ANY_POINTER:
        out += "Capnp.readAnyPointerChecked (" + pointerExpr() + ")";
        break;
      case schema::Type::LIST: {
        std::string ptrExpr = "(" + pointerExpr() + ")";
        out += listReadExprChecked(type.asList().getElementType(), ptrExpr, currentModule);
        break;
      }
    }

    return out;
  }

  std::string genEnum(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    auto enumSchema = schema.asEnum();

    std::string out;
    out += "inductive ";
    out += name.cStr();
    out += " where\n";

    std::set<std::string> used;
    std::vector<std::string> ctorNames;
    ctorNames.reserve(enumSchema.getEnumerants().size());
    for (auto enumerant: enumSchema.getEnumerants()) {
      auto rawName = sanitizeIdentifier(enumerant.getProto().getName());
      auto ctor = uniqueName(rawName.cStr(), used);
      ctorNames.push_back(ctor);
      out += "  | ";
      out += ctor;
      out += "\n";
    }
    std::string unknownName = "unknown";
    if (used.count(unknownName) != 0) {
      unknownName = uniqueName(unknownName, used);
    } else {
      used.insert(unknownName);
    }
    out += "  | ";
    out += unknownName;
    out += " (value : UInt16)\n\n";
    out += "  deriving BEq, Repr\n\n";

    out += "def ";
    out += name.cStr();
    out += ".toUInt16 : ";
    out += name.cStr();
    out += " -> UInt16\n";

    for (size_t i = 0; i < enumSchema.getEnumerants().size(); ++i) {
      auto ctor = ctorNames[i];
      out += "  | ";
      out += name.cStr();
      out += ".";
      out += ctor;
      out += " => ";
      out += std::to_string(i);
      out += "\n";
    }
    out += "  | ";
    out += name.cStr();
    out += ".";
    out += unknownName;
    out += " v => v\n\n";

    out += "def ";
    out += name.cStr();
    out += ".ofUInt16 : UInt16 -> ";
    out += name.cStr();
    out += "\n";

    for (size_t i = 0; i < enumSchema.getEnumerants().size(); ++i) {
      auto ctor = ctorNames[i];
      out += "  | ";
      out += std::to_string(i);
      out += " => ";
      out += name.cStr();
      out += ".";
      out += ctor;
      out += "\n";
    }
    out += "  | v => ";
    out += name.cStr();
    out += ".";
    out += unknownName;
    out += " v\n";

    auto nodeAnn = genAnnotationUses(name, schema.getProto().getAnnotations(), currentModule,
                                     kj::StringPtr(""));
    if (!nodeAnn.empty()) {
      out += "\n";
      out += nodeAnn;
    }

    for (size_t i = 0; i < enumSchema.getEnumerants().size(); ++i) {
      auto enumerant = enumSchema.getEnumerants()[i];
      auto annOut = genAnnotationUses(name, enumerant.getProto().getAnnotations(), currentModule,
                                      kj::StringPtr(ctorNames[i].c_str()));
      if (!annOut.empty()) {
        out += "\n";
        out += annOut;
      }
    }

    return out;
  }

  void collectStructSchemas(Schema schema,
                            std::vector<Schema>& out,
                            std::unordered_set<uint64_t>& seen) {
    auto proto = schema.getProto();
    if (proto.which() == schema::Node::STRUCT) {
      auto id = proto.getId();
      if (seen.insert(id).second) {
        out.push_back(schema);
      }
      auto structSchema = schema.asStruct();
      for (auto field: structSchema.getFields()) {
        if (!field.getProto().isGroup()) continue;
        auto groupId = field.getProto().getGroup().getTypeId();
        collectStructSchemas(schemaLoader.get(groupId), out, seen);
      }
    }
    if (proto.which() == schema::Node::INTERFACE) {
      auto interfaceSchema = schema.asInterface();
      for (auto method: interfaceSchema.getMethods()) {
        collectStructSchemas(method.getParamType(), out, seen);
        collectStructSchemas(method.getResultType(), out, seen);
      }
    }
    for (auto nested: proto.getNestedNodes()) {
      collectStructSchemas(schemaLoader.get(nested.getId()), out, seen);
    }
  }

  void collectStructDepsFromType(Type type, std::unordered_set<uint64_t>& deps) {
    switch (type.which()) {
      case schema::Type::STRUCT:
        deps.insert(type.asStruct().getProto().getId());
        break;
      case schema::Type::LIST:
        collectStructDepsFromType(type.asList().getElementType(), deps);
        break;
      default:
        break;
    }
  }

  std::unordered_set<uint64_t> collectStructDeps(StructSchema schema) {
    std::unordered_set<uint64_t> deps;
    for (auto field: schema.getFields()) {
      collectStructDepsFromType(field.getType(), deps);
    }
    return deps;
  }

  std::unordered_set<uint64_t> findRecursiveStructs(const std::vector<Schema>& structSchemas) {
    std::unordered_set<uint64_t> recursive;
    if (structSchemas.empty()) return recursive;

    std::unordered_map<uint64_t, size_t> indexById;
    indexById.reserve(structSchemas.size());
    for (size_t i = 0; i < structSchemas.size(); ++i) {
      indexById[structSchemas[i].getProto().getId()] = i;
    }

    std::vector<std::vector<size_t>> edges(structSchemas.size());
    for (size_t i = 0; i < structSchemas.size(); ++i) {
      auto deps = collectStructDeps(structSchemas[i].asStruct());
      for (auto depId: deps) {
        auto it = indexById.find(depId);
        if (it != indexById.end()) {
          edges[i].push_back(it->second);
        }
      }
    }

    for (size_t i = 0; i < structSchemas.size(); ++i) {
      std::vector<char> seen(structSchemas.size(), 0);
      auto dfs = [&](auto&& self, size_t cur) -> bool {
        if (seen[cur]) return false;
        seen[cur] = 1;
        for (auto nxt: edges[cur]) {
          if (nxt == i) return true;
          if (self(self, nxt)) return true;
        }
        return false;
      };
      if (dfs(dfs, i)) {
        recursive.insert(structSchemas[i].getProto().getId());
      }
    }

    return recursive;
  }

  std::string genStructPrelude(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    std::string out;
    out += "structure ";
    out += name.cStr();
    out += ".Reader where\n";
    out += "  struct : Capnp.StructReader\n\n";
    out += "  deriving BEq\n\n";
    out += "def ";
    out += name.cStr();
    out += ".read (ptr : Capnp.AnyPointer) : ";
    out += name.cStr();
    out += ".Reader := { struct := Capnp.readStruct ptr }\n";
    out += "def ";
    out += name.cStr();
    out += ".readChecked (ptr : Capnp.AnyPointer) : Except String ";
    out += name.cStr();
    out += ".Reader := do\n";
    out += "  let sr ← Capnp.readStructChecked ptr\n";
    out += "  return { struct := sr }\n";
    out += "def ";
    out += name.cStr();
    out += ".fromStruct (sr : Capnp.StructReader) : ";
    out += name.cStr();
    out += ".Reader := { struct := sr }\n\n";

    out += "structure ";
    out += name.cStr();
    out += ".Builder where\n";
    out += "  struct : Capnp.StructBuilder\n\n";
    out += "  deriving BEq\n\n";
    out += "def ";
    out += name.cStr();
    out += ".Builder.fromStruct (sb : Capnp.StructBuilder) : ";
    out += name.cStr();
    out += ".Builder := { struct := sb }\n";
    return out;
  }

  std::string genStructAlias(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    std::string out;
    out += "abbrev ";
    out += name.cStr();
    out += " := ";
    out += name.cStr();
    out += ".Reader";
    return out;
  }

  std::string genStructUnion(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    auto structSchema = schema.asStruct();
    auto unionFields = structSchema.getUnionFields();
    if (unionFields.size() == 0) return "";

    auto info = buildUnionInfo(structSchema);
    std::string out;
    out += "inductive ";
    out += name.cStr();
    out += ".Which where\n";
    out += "  | ";
    out += info.noneName;
    out += "\n";
    for (auto& entry: info.cases) {
      auto field = entry.first;
      auto ctor = entry.second;
      auto fieldType = typeToLeanReader(field.getType(), currentModule);
      out += "  | ";
      out += ctor;
      out += " (value : ";
      out += fieldType;
      out += ")\n";
    }
    out += "\n";
    out += "instance : BEq ";
    out += name.cStr();
    out += ".Which where\n";
    out += "  beq a b :=\n";
    out += "    match a, b with\n";
    out += "    | ";
    out += name.cStr();
    out += ".Which.";
    out += info.noneName;
    out += ", ";
    out += name.cStr();
    out += ".Which.";
    out += info.noneName;
    out += " => true\n";
    for (auto& entry: info.cases) {
      auto field = entry.first;
      auto ctor = entry.second;
      out += "    | ";
      out += name.cStr();
      out += ".Which.";
      out += ctor;
      out += " va, ";
      out += name.cStr();
      out += ".Which.";
      out += ctor;
      out += " vb => ";
      if (typeSupportsBeq(field.getType())) {
        out += "va == vb\n";
      } else {
        out += "true\n";
      }
    }
    out += "    | _, _ => false\n\n";
    return out;
  }

  std::string genStructType(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    auto structSchema = schema.asStruct();
    auto unionFields = structSchema.getUnionFields();
    auto nonUnionFields = structSchema.getNonUnionFields();

    std::string out;
    out += "structure ";
    out += name.cStr();
    out += " where\n";

    std::set<std::string> usedFields;
    for (auto field: nonUnionFields) {
      auto rawName = sanitizeIdentifier(field.getProto().getName());
      auto fieldName = uniqueName(rawName.cStr(), usedFields);
      auto fieldType = typeToLean(field.getType(), currentModule);
      out += "  ";
      out += fieldName;
      out += " : ";
      out += fieldType;
      out += "\n";
    }

    if (unionFields.size() > 0) {
      auto whichName = uniqueName("which", usedFields);
      out += "  ";
      out += whichName;
      out += " : ";
      out += name.cStr();
      out += ".Which\n";
    }
    return out;
  }

  std::string genStruct(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    auto structSchema = schema.asStruct();
    auto unionFields = structSchema.getUnionFields();
    std::unordered_set<uint> unionFieldIndex;
    for (auto field: unionFields) {
      unionFieldIndex.insert(field.getIndex());
    }

    std::string out;
    std::vector<std::pair<StructSchema::Field, std::string>> unionCases;
    std::string unionNoneName;
    if (unionFields.size() > 0) {
      auto info = buildUnionInfo(structSchema);
      unionNoneName = info.noneName;
      unionCases = info.cases;
    }

    std::set<std::string> usedAccessors;
    std::unordered_map<uint, std::string> accessorByIndex;
    for (auto field: structSchema.getFields()) {
      auto raw = sanitizeIdentifier(field.getProto().getName());
      auto base = capitalizeIdentifier(raw);
      auto baseName = std::string("get") + base.cStr();
      auto unique = uniqueName(baseName, usedAccessors);
      accessorByIndex[field.getIndex()] = unique;
    }

    std::unordered_map<uint, std::string> checkedAccessorByIndex;
    for (auto field: structSchema.getFields()) {
      auto accIt = accessorByIndex.find(field.getIndex());
      auto baseName = accIt == accessorByIndex.end()
          ? std::string("getChecked")
          : accIt->second + "Checked";
      auto unique = uniqueName(baseName, usedAccessors);
      checkedAccessorByIndex[field.getIndex()] = unique;
    }

    std::set<std::string> usedHas;
    std::unordered_map<uint, std::string> hasByIndex;
    for (auto field: structSchema.getFields()) {
      if (field.getProto().isGroup()) continue;
      if (!isPointerType(field.getType())) continue;
      auto raw = sanitizeIdentifier(field.getProto().getName());
      auto base = capitalizeIdentifier(raw);
      auto baseName = std::string("has") + base.cStr();
      auto unique = uniqueName(baseName, usedHas);
      hasByIndex[field.getIndex()] = unique;
    }

    std::unordered_map<uint, std::string> defaultPointers;
    std::set<std::string> usedDefaults;
    for (auto field: structSchema.getFields()) {
      if (field.getProto().isGroup()) continue;
      auto type = field.getType();
      if (!isPointerType(type)) continue;
      auto slot = field.getProto().getSlot();
      auto literal = defaultPointerLiteral(type, slot.getDefaultValue());
      if (literal == kj::none) continue;
      auto rawName = sanitizeIdentifier(field.getProto().getName());
      auto baseName = std::string("_default_") + rawName.cStr();
      auto unique = uniqueName(baseName, usedDefaults);
      auto fullName = std::string(name.cStr()) + "." + unique;
      defaultPointers[field.getIndex()] = fullName;
      out += "\n";
      out += "def ";
      out += fullName;
      out += " : Capnp.AnyPointer :=\n";
      out += "  Capnp.getRoot (Capnp.readMessage (";
      out += KJ_ASSERT_NONNULL(literal);
      out += "))\n";
    }

    for (auto field: structSchema.getFields()) {
      auto it = accessorByIndex.find(field.getIndex());
      auto accessor = it == accessorByIndex.end() ? std::string("get") : it->second;
      auto itChecked = checkedAccessorByIndex.find(field.getIndex());
      auto accessorChecked = itChecked == checkedAccessorByIndex.end()
          ? std::string("getChecked")
          : itChecked->second;
      std::string defaultPtrExpr;
      auto defIt = defaultPointers.find(field.getIndex());
      if (defIt != defaultPointers.end()) {
        defaultPtrExpr = defIt->second;
      }
      out += "\n";
      out += genFieldAccessor(field, name, currentModule, accessor, defaultPointers);
      out += "\n";
      out += genFieldAccessorChecked(field, name, currentModule, accessorChecked, defaultPointers);
      out += "\n";
      if (!field.getProto().isGroup()) {
        auto type = field.getType();
        if (type.which() == schema::Type::TEXT || type.which() == schema::Type::DATA) {
          auto offset = field.getProto().getSlot().getOffset();
          std::string viewType = type.which() == schema::Type::TEXT
              ? "Capnp.TextView"
              : "Capnp.DataView";
          std::string readFn = type.which() == schema::Type::TEXT
              ? "Capnp.readTextView"
              : "Capnp.readDataView";
          std::string readFnChecked = type.which() == schema::Type::TEXT
              ? "Capnp.readTextViewChecked"
              : "Capnp.readDataViewChecked";

          std::string viewName = uniqueName(accessor + "View", usedAccessors);
          out += "def ";
          out += name.cStr();
          out += ".Reader.";
          out += viewName;
          out += " (r : ";
          out += name.cStr();
          out += ".Reader) : ";
          out += viewType;
          out += " := ";
          if (defaultPtrExpr.empty()) {
            out += readFn;
            out += " (Capnp.getPointer r.struct ";
            out += std::to_string(offset);
            out += ")\n";
          } else {
            out += readFn;
            out += " (Capnp.withDefaultPointer (Capnp.getPointer r.struct ";
            out += std::to_string(offset);
            out += ") ";
            out += defaultPtrExpr;
            out += ")\n";
          }

          std::string checkedName = uniqueName(accessor + "ViewChecked", usedAccessors);
          out += "def ";
          out += name.cStr();
          out += ".Reader.";
          out += checkedName;
          out += " (r : ";
          out += name.cStr();
          out += ".Reader) : Except String ";
          out += viewType;
          out += " := ";
          if (defaultPtrExpr.empty()) {
            out += readFnChecked;
            out += " (Capnp.getPointer r.struct ";
            out += std::to_string(offset);
            out += ")\n\n";
          } else {
            out += readFnChecked;
            out += " (Capnp.withDefaultPointer (Capnp.getPointer r.struct ";
            out += std::to_string(offset);
            out += ") ";
            out += defaultPtrExpr;
            out += ")\n\n";
          }
        }
      }
      if (!field.getProto().isGroup() && isPointerType(field.getType())) {
        auto hit = hasByIndex.find(field.getIndex());
        auto hasName = hit == hasByIndex.end() ? std::string("has") : hit->second;
        auto offset = field.getProto().getSlot().getOffset();
        out += "def ";
        out += name.cStr();
        out += ".Reader.";
        out += hasName;
        out += " (r : ";
        out += name.cStr();
        out += ".Reader) : Bool := !Capnp.isNullPointer (Capnp.getPointer r.struct ";
        out += std::to_string(offset);
        out += ")\n\n";
      }
    }

    if (unionFields.size() > 0) {
      auto discOffset = structSchema.getProto().getStruct().getDiscriminantOffset();
      auto discByteOffset = discOffset * 2;
      out += "\n";
      out += "def ";
      out += name.cStr();
      out += ".Reader.which (r : ";
      out += name.cStr();
      out += ".Reader) : ";
      out += name.cStr();
      out += ".Which :=\n";
      out += "  let disc := Capnp.getUInt16 r.struct ";
      out += std::to_string(discByteOffset);
      out += "\n  match disc with\n";

      for (auto& entry: unionCases) {
        auto field = entry.first;
        auto ctor = entry.second;
        auto it = accessorByIndex.find(field.getIndex());
        auto accessorName = it == accessorByIndex.end() ? std::string("get") : it->second;
        out += "  | ";
        out += std::to_string(field.getProto().getDiscriminantValue());
        out += " => ";
        out += name.cStr();
        out += ".Which.";
        out += ctor;
        out += " (r.";
        out += accessorName;
        out += ")\n";
      }

      out += "  | _ => ";
      out += name.cStr();
      out += ".Which.";
      out += unionNoneName;
      out += "\n";

      out += "\n";
      out += "def ";
      out += name.cStr();
      out += ".Reader.whichChecked (r : ";
      out += name.cStr();
      out += ".Reader) : Except String ";
      out += name.cStr();
      out += ".Which := do\n";
      out += "  let disc := Capnp.getUInt16 r.struct ";
      out += std::to_string(discByteOffset);
      out += "\n  match disc with\n";

      for (auto& entry: unionCases) {
        auto field = entry.first;
        auto ctor = entry.second;
        auto it = checkedAccessorByIndex.find(field.getIndex());
        auto accessorName = it == checkedAccessorByIndex.end()
            ? std::string("getChecked")
            : it->second;
        out += "  | ";
        out += std::to_string(field.getProto().getDiscriminantValue());
        out += " => do\n";
        out += "    let v ← r.";
        out += accessorName;
        out += "\n";
        out += "    return ";
        out += name.cStr();
        out += ".Which.";
        out += ctor;
        out += " v\n";
      }

      out += "  | _ => return ";
      out += name.cStr();
      out += ".Which.";
      out += unionNoneName;
      out += "\n";
    }

    auto dataWords = structSchema.getProto().getStruct().getDataWordCount();
    auto ptrCount = structSchema.getProto().getStruct().getPointerCount();

    out += "def ";
    out += name.cStr();
    out += ".initRoot : Capnp.BuilderM ";
    out += name.cStr();
    out += ".Builder := do\n";
    out += "  let p ← Capnp.getRootPointer\n";
    out += "  let sb ← Capnp.initStructPointer p ";
    out += std::to_string(dataWords);
    out += " ";
    out += std::to_string(ptrCount);
    out += "\n  return { struct := sb }\n";

    std::set<std::string> usedSetters;
    std::set<std::string> usedInits;
    std::set<std::string> usedClears;
    std::unordered_map<uint, std::string> setterByIndex;
    std::unordered_map<uint, std::string> readerSetterByIndex;
    std::unordered_map<uint, std::string> initByIndex;
    std::unordered_map<uint, std::string> clearByIndex;
    for (auto field: structSchema.getFields()) {
      auto raw = sanitizeIdentifier(field.getProto().getName());
      auto base = capitalizeIdentifier(raw);
      auto setName = std::string("set") + base.cStr();
      auto initName = std::string("init") + base.cStr();
      setterByIndex[field.getIndex()] = uniqueName(setName, usedSetters);
      readerSetterByIndex[field.getIndex()] = uniqueName(setName + "FromReaders", usedSetters);
      initByIndex[field.getIndex()] = uniqueName(initName, usedInits);
      if (!field.getProto().isGroup() && isPointerType(field.getType())) {
        auto clearName = std::string("clear") + base.cStr();
        clearByIndex[field.getIndex()] = uniqueName(clearName, usedClears);
      }
    }

    uint discByteOffset = 0;
    if (unionFields.size() > 0) {
      auto discOffset = structSchema.getProto().getStruct().getDiscriminantOffset();
      discByteOffset = discOffset * 2;
    }

    for (auto field: structSchema.getFields()) {
      auto isUnion = unionFieldIndex.count(field.getIndex()) != 0;
      auto itSet = setterByIndex.find(field.getIndex());
      auto setterName = itSet == setterByIndex.end() ? std::string("set") : itSet->second;
      auto itSetReader = readerSetterByIndex.find(field.getIndex());
      auto readerSetterName = itSetReader == readerSetterByIndex.end()
          ? setterName + "FromReaders"
          : itSetReader->second;
      auto itInit = initByIndex.find(field.getIndex());
      auto initName = itInit == initByIndex.end() ? std::string("init") : itInit->second;
      auto itClear = clearByIndex.find(field.getIndex());
      auto clearName = itClear == clearByIndex.end() ? std::string("clear") : itClear->second;
      auto itAcc = accessorByIndex.find(field.getIndex());
      auto accessorName = itAcc == accessorByIndex.end() ? std::string("get") : itAcc->second;

      if (field.getProto().isGroup()) {
        auto groupTypeId = field.getProto().getGroup().getTypeId();
        auto groupName = qualifiedTypeName(groupTypeId, currentModule);
        out += "\n";
        out += "def ";
        out += name.cStr();
        out += ".Builder.";
        out += accessorName;
        out += " (b : ";
        out += name.cStr();
        out += ".Builder) : ";
        out += groupName.cStr();
        out += ".Builder := ";
        out += groupName.cStr();
        out += ".Builder.fromStruct b.struct\n";
        continue;
      }

      auto slot = field.getProto().getSlot();
      auto offset = slot.getOffset();
      auto defaultValue = slot.getDefaultValue();
      auto type = field.getType();

      if (isPointerType(type)) {
        out += "\n";
        out += "def ";
        out += name.cStr();
        out += ".Builder.";
        out += clearName;
        out += " (b : ";
        out += name.cStr();
        out += ".Builder) : Capnp.BuilderM Unit := do\n";
        out += "  Capnp.clearPointer (Capnp.getPointerBuilder b.struct ";
        out += std::to_string(offset);
        out += ")\n";
      }

      switch (type.which()) {
        case schema::Type::VOID: {
          auto builderVar = isUnion ? std::string("b") : std::string("_b");
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (";
          out += builderVar;
          out += " : ";
          out += name.cStr();
          out += ".Builder) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          out += "  pure ()\n";
          break;
        }
        case schema::Type::BOOL: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Bool) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getBool()) {
            out += "  Capnp.setBoolMasked b.struct ";
            out += std::to_string(offset);
            out += " true v\n";
          } else {
            out += "  Capnp.setBool b.struct ";
            out += std::to_string(offset);
            out += " v\n";
          }
          break;
        }
        case schema::Type::INT8: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Int8) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getInt8() != 0) {
            uint8_t mask = static_cast<uint8_t>(defaultValue.getInt8());
            out += "  Capnp.setInt8Masked b.struct ";
            out += std::to_string(offset);
            out += " (UInt8.ofNat ";
            out += std::to_string(mask);
            out += ") v\n";
          } else {
            out += "  Capnp.setInt8 b.struct ";
            out += std::to_string(offset);
            out += " v\n";
          }
          break;
        }
        case schema::Type::UINT8: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : UInt8) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getUint8() != 0) {
            out += "  Capnp.setUInt8Masked b.struct ";
            out += std::to_string(offset);
            out += " (UInt8.ofNat ";
            out += std::to_string(defaultValue.getUint8());
            out += ") v\n";
          } else {
            out += "  Capnp.setUInt8 b.struct ";
            out += std::to_string(offset);
            out += " v\n";
          }
          break;
        }
        case schema::Type::INT16: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Int16) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getInt16() != 0) {
            uint16_t mask = static_cast<uint16_t>(defaultValue.getInt16());
            out += "  Capnp.setInt16Masked b.struct ";
            out += std::to_string(offset * 2);
            out += " (UInt16.ofNat ";
            out += std::to_string(mask);
            out += ") v\n";
          } else {
            out += "  Capnp.setInt16 b.struct ";
            out += std::to_string(offset * 2);
            out += " v\n";
          }
          break;
        }
        case schema::Type::UINT16: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : UInt16) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getUint16() != 0) {
            out += "  Capnp.setUInt16Masked b.struct ";
            out += std::to_string(offset * 2);
            out += " (UInt16.ofNat ";
            out += std::to_string(defaultValue.getUint16());
            out += ") v\n";
          } else {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(offset * 2);
            out += " v\n";
          }
          break;
        }
        case schema::Type::INT32: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Int32) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getInt32() != 0) {
            uint32_t mask = static_cast<uint32_t>(defaultValue.getInt32());
            out += "  Capnp.setInt32Masked b.struct ";
            out += std::to_string(offset * 4);
            out += " (UInt32.ofNat ";
            out += std::to_string(mask);
            out += ") v\n";
          } else {
            out += "  Capnp.setInt32 b.struct ";
            out += std::to_string(offset * 4);
            out += " v\n";
          }
          break;
        }
        case schema::Type::UINT32: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : UInt32) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getUint32() != 0) {
            out += "  Capnp.setUInt32Masked b.struct ";
            out += std::to_string(offset * 4);
            out += " (UInt32.ofNat ";
            out += std::to_string(defaultValue.getUint32());
            out += ") v\n";
          } else {
            out += "  Capnp.setUInt32 b.struct ";
            out += std::to_string(offset * 4);
            out += " v\n";
          }
          break;
        }
        case schema::Type::INT64: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Int64) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getInt64() != 0) {
            uint64_t mask = static_cast<uint64_t>(defaultValue.getInt64());
            out += "  Capnp.setInt64Masked b.struct ";
            out += std::to_string(offset * 8);
            out += " (UInt64.ofNat ";
            out += std::to_string(mask);
            out += ") v\n";
          } else {
            out += "  Capnp.setInt64 b.struct ";
            out += std::to_string(offset * 8);
            out += " v\n";
          }
          break;
        }
        case schema::Type::UINT64: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : UInt64) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getUint64() != 0) {
            out += "  Capnp.setUInt64Masked b.struct ";
            out += std::to_string(offset * 8);
            out += " (UInt64.ofNat ";
            out += std::to_string(defaultValue.getUint64());
            out += ") v\n";
          } else {
            out += "  Capnp.setUInt64 b.struct ";
            out += std::to_string(offset * 8);
            out += " v\n";
          }
          break;
        }
        case schema::Type::FLOAT32: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Float) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getFloat32() != 0.0f) {
            uint32_t mask;
            float v = defaultValue.getFloat32();
            memcpy(&mask, &v, sizeof(mask));
            out += "  Capnp.setFloat32Masked b.struct ";
            out += std::to_string(offset * 4);
            out += " (UInt32.ofNat ";
            out += std::to_string(mask);
            out += ") v\n";
          } else {
            out += "  Capnp.setFloat32 b.struct ";
            out += std::to_string(offset * 4);
            out += " v\n";
          }
          break;
        }
        case schema::Type::FLOAT64: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Float) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getFloat64() != 0.0) {
            uint64_t mask;
            double v = defaultValue.getFloat64();
            memcpy(&mask, &v, sizeof(mask));
            out += "  Capnp.setFloat64Masked b.struct ";
            out += std::to_string(offset * 8);
            out += " (UInt64.ofNat ";
            out += std::to_string(mask);
            out += ") v\n";
          } else {
            out += "  Capnp.setFloat64 b.struct ";
            out += std::to_string(offset * 8);
            out += " v\n";
          }
          break;
        }
        case schema::Type::ENUM: {
          auto enumName = qualifiedTypeName(type.asEnum().getProto().getId(), currentModule);
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : ";
          out += enumName.cStr();
          out += ") : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          if (defaultValue.getEnum() != 0) {
            out += "  Capnp.setUInt16Masked b.struct ";
            out += std::to_string(offset * 2);
            out += " (UInt16.ofNat ";
            out += std::to_string(defaultValue.getEnum());
            out += ") (";
            out += enumName.cStr();
            out += ".toUInt16 v)\n";
          } else {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(offset * 2);
            out += " (";
            out += enumName.cStr();
            out += ".toUInt16 v)\n";
          }
          break;
        }
        case schema::Type::TEXT: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Capnp.Text) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          out += "  Capnp.writeText (Capnp.getPointerBuilder b.struct ";
          out += std::to_string(offset);
          out += ") v\n";
          break;
        }
        case schema::Type::DATA: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Capnp.Data) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          out += "  Capnp.writeData (Capnp.getPointerBuilder b.struct ";
          out += std::to_string(offset);
          out += ") v\n";
          break;
        }
        case schema::Type::STRUCT: {
          auto structName = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += initName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) : Capnp.BuilderM ";
          out += structName.cStr();
          out += ".Builder := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          auto targetSchema = type.asStruct();
          auto dataWords = targetSchema.getProto().getStruct().getDataWordCount();
          auto ptrCount = targetSchema.getProto().getStruct().getPointerCount();
          out += "  let sb ← Capnp.initStructPointer (Capnp.getPointerBuilder b.struct ";
          out += std::to_string(offset);
          out += ") ";
          out += std::to_string(dataWords);
          out += " ";
          out += std::to_string(ptrCount);
          out += "\n  return ";
          out += structName.cStr();
          out += ".Builder.fromStruct sb\n";
          break;
        }
        case schema::Type::LIST: {
          auto elemType = type.asList().getElementType();
          out += "\n";
          if (elemType.which() == schema::Type::STRUCT) {
            auto elemName = qualifiedTypeName(elemType.asStruct().getProto().getId(), currentModule);
            auto elemSchema = elemType.asStruct();
            auto elemData = elemSchema.getProto().getStruct().getDataWordCount();
            auto elemPtrs = elemSchema.getProto().getStruct().getPointerCount();
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += initName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (n : Nat) : Capnp.BuilderM (Array ";
            out += elemName.cStr();
            out += ".Builder) := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  let lb ← Capnp.initStructListPointer (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") ";
            out += std::to_string(elemData);
            out += " ";
            out += std::to_string(elemPtrs);
            out += " n\n";
            out += "  return Array.map ";
            out += elemName.cStr();
            out += ".Builder.fromStruct (Capnp.listStructBuilders lb)\n";

            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += readerSetterName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (v : Array ";
            out += elemName.cStr();
            out += ".Reader) : Capnp.BuilderM Unit := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            emitListWrite(elemType,
                          "Capnp.getPointerBuilder b.struct " + std::to_string(offset),
                          "v",
                          currentModule,
                          out,
                          "  ",
                          true);
        } else if (elemType.which() == schema::Type::LIST ||
                   elemType.which() == schema::Type::ANY_POINTER) {
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += initName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (n : Nat) : Capnp.BuilderM (Array Capnp.AnyPointerBuilder) := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListPointer (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") n\n";

            auto readerStructs = listContainsStruct(elemType);
            auto elemTypeName = listSetterElemType(elemType, currentModule, false);
            auto arrayType = elemTypeName.find(' ') != std::string::npos
                ? "Array (" + elemTypeName + ")"
                : "Array " + elemTypeName;
            if (readerStructs) {
              auto readerElemType = listSetterElemType(elemType, currentModule, true);
              auto readerArrayType = readerElemType.find(' ') != std::string::npos
                  ? "Array (" + readerElemType + ")"
                  : "Array " + readerElemType;
              out += "def ";
              out += name.cStr();
              out += ".Builder.";
              out += readerSetterName;
              out += " (b : ";
              out += name.cStr();
              out += ".Builder) (v : ";
              out += readerArrayType;
              out += ") : Capnp.BuilderM Unit := do\n";
              if (isUnion) {
                out += "  Capnp.setUInt16 b.struct ";
                out += std::to_string(discByteOffset);
                out += " (UInt16.ofNat ";
                out += std::to_string(field.getProto().getDiscriminantValue());
                out += ")\n";
              }
              emitListWrite(elemType,
                            "Capnp.getPointerBuilder b.struct " + std::to_string(offset),
                            "v",
                            currentModule,
                            out,
                            "  ",
                            true);
            } else {
              out += "def ";
              out += name.cStr();
              out += ".Builder.";
              out += setterName;
              out += " (b : ";
              out += name.cStr();
              out += ".Builder) (v : ";
              out += arrayType;
              out += ") : Capnp.BuilderM Unit := do\n";
              if (isUnion) {
                out += "  Capnp.setUInt16 b.struct ";
                out += std::to_string(discByteOffset);
                out += " (UInt16.ofNat ";
                out += std::to_string(field.getProto().getDiscriminantValue());
                out += ")\n";
              }
              emitListWrite(elemType,
                            "Capnp.getPointerBuilder b.struct " + std::to_string(offset),
                            "v",
                            currentModule,
                            out,
                            "  ",
                            false);
            }
          } else if (elemType.which() == schema::Type::INTERFACE) {
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += initName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (n : Nat) : Capnp.BuilderM (Array Capnp.AnyPointerBuilder) := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListPointer (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") n\n";

            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += setterName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (v : Array Capnp.Capability) : Capnp.BuilderM Unit := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListCapability (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") v\n";
          } else if (elemType.which() == schema::Type::TEXT) {
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += initName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (n : Nat) : Capnp.BuilderM (Array Capnp.AnyPointerBuilder) := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListPointer (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") n\n";

            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += setterName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (v : Array Capnp.Text) : Capnp.BuilderM Unit := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListText (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") v\n";
          } else if (elemType.which() == schema::Type::DATA) {
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += initName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (n : Nat) : Capnp.BuilderM (Array Capnp.AnyPointerBuilder) := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListPointer (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") n\n";

            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += setterName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (v : Array Capnp.Data) : Capnp.BuilderM Unit := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListData (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") v\n";
          } else if (elemType.which() == schema::Type::ENUM) {
            auto enumName = qualifiedTypeName(elemType.asEnum().getProto().getId(), currentModule);
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += initName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (n : Nat) : Capnp.BuilderM Capnp.ListBuilder := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.initListPointer (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") Capnp.elemSizeTwoBytes n\n";

            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += setterName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (v : Array ";
            out += enumName.cStr();
            out += ") : Capnp.BuilderM Unit := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.writeListUInt16 (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") (Array.map ";
            out += enumName.cStr();
            out += ".toUInt16 v)\n";
          } else {
            std::string elemSizeExpr = "Capnp.elemSizePointer";
            switch (elemType.which()) {
              case schema::Type::VOID: elemSizeExpr = "Capnp.elemSizeVoid"; break;
              case schema::Type::BOOL: elemSizeExpr = "Capnp.elemSizeBit"; break;
              case schema::Type::INT8: elemSizeExpr = "Capnp.elemSizeByte"; break;
              case schema::Type::UINT8: elemSizeExpr = "Capnp.elemSizeByte"; break;
              case schema::Type::INT16: elemSizeExpr = "Capnp.elemSizeTwoBytes"; break;
              case schema::Type::UINT16: elemSizeExpr = "Capnp.elemSizeTwoBytes"; break;
              case schema::Type::INT32: elemSizeExpr = "Capnp.elemSizeFourBytes"; break;
              case schema::Type::UINT32: elemSizeExpr = "Capnp.elemSizeFourBytes"; break;
              case schema::Type::INT64: elemSizeExpr = "Capnp.elemSizeEightBytes"; break;
              case schema::Type::UINT64: elemSizeExpr = "Capnp.elemSizeEightBytes"; break;
              case schema::Type::FLOAT32: elemSizeExpr = "Capnp.elemSizeFourBytes"; break;
              case schema::Type::FLOAT64: elemSizeExpr = "Capnp.elemSizeEightBytes"; break;
              default: break;
            }
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += initName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (n : Nat) : Capnp.BuilderM Capnp.ListBuilder := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.initListPointer (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") ";
            out += elemSizeExpr;
            out += " n\n";

            auto elemTypeName = typeToLean(elemType, currentModule);
            auto arrayType = elemTypeName.find(' ') != std::string::npos
                ? "Array (" + elemTypeName + ")"
                : "Array " + elemTypeName;
            out += "def ";
            out += name.cStr();
            out += ".Builder.";
            out += setterName;
            out += " (b : ";
            out += name.cStr();
            out += ".Builder) (v : ";
            out += arrayType;
            out += ") : Capnp.BuilderM Unit := do\n";
            if (isUnion) {
              out += "  Capnp.setUInt16 b.struct ";
              out += std::to_string(discByteOffset);
              out += " (UInt16.ofNat ";
              out += std::to_string(field.getProto().getDiscriminantValue());
              out += ")\n";
            }
            out += "  Capnp.";
            switch (elemType.which()) {
              case schema::Type::VOID: out += "writeListVoid"; break;
              case schema::Type::BOOL: out += "writeListBool"; break;
              case schema::Type::INT8: out += "writeListInt8"; break;
              case schema::Type::INT16: out += "writeListInt16"; break;
              case schema::Type::INT32: out += "writeListInt32"; break;
              case schema::Type::INT64: out += "writeListInt64"; break;
              case schema::Type::UINT8: out += "writeListUInt8"; break;
              case schema::Type::UINT16: out += "writeListUInt16"; break;
              case schema::Type::UINT32: out += "writeListUInt32"; break;
              case schema::Type::UINT64: out += "writeListUInt64"; break;
              case schema::Type::FLOAT32: out += "writeListFloat32"; break;
              case schema::Type::FLOAT64: out += "writeListFloat64"; break;
              default: out += "writeListPointer"; break;
            }
            out += " (Capnp.getPointerBuilder b.struct ";
            out += std::to_string(offset);
            out += ") v\n";
          }
          break;
        }
        case schema::Type::ANY_POINTER: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += accessorName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) : Capnp.AnyPointerBuilder := ";
          out += "Capnp.getPointerBuilder b.struct ";
          out += std::to_string(offset);
          out += "\n";
          break;
        }
        case schema::Type::INTERFACE: {
          out += "\n";
          out += "def ";
          out += name.cStr();
          out += ".Builder.";
          out += setterName;
          out += " (b : ";
          out += name.cStr();
          out += ".Builder) (v : Capnp.Capability) : Capnp.BuilderM Unit := do\n";
          if (isUnion) {
            out += "  Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          out += "  Capnp.writeCapability (Capnp.getPointerBuilder b.struct ";
          out += std::to_string(offset);
          out += ") v\n";
          break;
        }
        default:
          break;
      }
    }

    auto nodeAnnOut = genAnnotationUses(name, schema.getProto().getAnnotations(),
                                        currentModule, kj::StringPtr(""));
    if (!nodeAnnOut.empty()) {
      out += "\n";
      out += nodeAnnOut;
    }

    for (auto field: structSchema.getFields()) {
      auto prefix = sanitizeIdentifier(field.getProto().getName());
      auto annOut = genAnnotationUses(name, field.getProto().getAnnotations(),
                                      currentModule, prefix);
      if (!annOut.empty()) {
        out += "\n";
        out += annOut;
      }
    }

    return out;
  }

  std::string genOfReader(Schema schema, kj::StringPtr currentModule, bool recursiveStruct) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    if (recursiveStruct) {
      std::string out;
      out += "def ";
      out += name.cStr();
      out += ".ofReader (r : ";
      out += name.cStr();
      out += ".Reader) : ";
      out += name.cStr();
      out += " := r\n";
      return out;
    }

    auto structSchema = schema.asStruct();
    auto unionFields = structSchema.getUnionFields();
    auto nonUnionFields = structSchema.getNonUnionFields();

    std::set<std::string> usedAccessors;
    std::unordered_map<uint, std::string> accessorByIndex;
    for (auto field: structSchema.getFields()) {
      auto raw = sanitizeIdentifier(field.getProto().getName());
      auto base = capitalizeIdentifier(raw);
      auto baseName = std::string("get") + base.cStr();
      auto unique = uniqueName(baseName, usedAccessors);
      accessorByIndex[field.getIndex()] = unique;
    }

    std::set<std::string> usedFields;
    std::unordered_map<uint, std::string> fieldNameByIndex;
    for (auto field: nonUnionFields) {
      auto rawName = sanitizeIdentifier(field.getProto().getName());
      auto fieldName = uniqueName(rawName.cStr(), usedFields);
      fieldNameByIndex[field.getIndex()] = fieldName;
    }

    std::string whichFieldName;
    if (unionFields.size() > 0) {
      whichFieldName = uniqueName("which", usedFields);
    }
    bool usesReaderVar = nonUnionFields.size() > 0 || unionFields.size() > 0;
    auto readerVar = usesReaderVar ? std::string("r") : std::string("_r");

    std::string out;
    out += "def ";
    out += name.cStr();
    out += ".ofReader (";
    out += readerVar;
    out += " : ";
    out += name.cStr();
    out += ".Reader) : ";
    out += name.cStr();
    out += " :=\n";
    out += "  { ";
    bool firstField = true;
    for (auto field: nonUnionFields) {
      auto nameIt = fieldNameByIndex.find(field.getIndex());
      auto accIt = accessorByIndex.find(field.getIndex());
      auto fieldName = nameIt == fieldNameByIndex.end() ? std::string("field") : nameIt->second;
      auto accessorName = accIt == accessorByIndex.end() ? std::string("get") : accIt->second;
      auto readerExpr = readerVar + "." + accessorName;
      auto valueExpr = readerToValueExpr(field.getType(), readerExpr, currentModule);
      if (!firstField) {
        out += "\n  , ";
      }
      out += fieldName;
      out += " := ";
      out += valueExpr;
      firstField = false;
    }
    if (unionFields.size() > 0) {
      if (!firstField) {
        out += "\n  , ";
      }
      out += whichFieldName;
      out += " := ";
      out += readerVar;
      out += ".which";
      firstField = false;
    }
    out += "\n  }\n";
    return out;
  }

  std::string genDeferredValueListSetters(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    auto structSchema = schema.asStruct();
    auto unionFields = structSchema.getUnionFields();

    std::unordered_set<uint> unionFieldIndex;
    for (auto field: unionFields) {
      unionFieldIndex.insert(field.getIndex());
    }

    std::set<std::string> usedSetters;
    std::unordered_map<uint, std::string> setterByIndex;
    for (auto field: structSchema.getFields()) {
      auto raw = sanitizeIdentifier(field.getProto().getName());
      auto base = capitalizeIdentifier(raw);
      auto setName = std::string("set") + base.cStr();
      setterByIndex[field.getIndex()] = uniqueName(setName, usedSetters);
      uniqueName(setName + "FromReaders", usedSetters);
    }

    uint discByteOffset = 0;
    if (unionFields.size() > 0) {
      auto discOffset = structSchema.getProto().getStruct().getDiscriminantOffset();
      discByteOffset = discOffset * 2;
    }

    std::string out;
    for (auto field: structSchema.getFields()) {
      if (field.getProto().isGroup()) continue;
      auto type = field.getType();
      if (type.which() != schema::Type::LIST) continue;

      auto elemType = type.asList().getElementType();
      auto needsDeferred = elemType.which() == schema::Type::STRUCT ||
          ((elemType.which() == schema::Type::LIST || elemType.which() == schema::Type::ANY_POINTER) &&
           listContainsStruct(elemType));
      if (!needsDeferred) continue;

      auto isUnion = unionFieldIndex.count(field.getIndex()) != 0;
      auto itSet = setterByIndex.find(field.getIndex());
      auto setterName = itSet == setterByIndex.end() ? std::string("set") : itSet->second;
      auto offset = field.getProto().getSlot().getOffset();

      out += "def ";
      out += name.cStr();
      out += ".Builder.";
      out += setterName;
      out += " (b : ";
      out += name.cStr();
      out += ".Builder) (v : ";
      if (elemType.which() == schema::Type::STRUCT) {
        auto elemName = qualifiedTypeName(elemType.asStruct().getProto().getId(), currentModule);
        out += "Array ";
        out += elemName.cStr();
      } else {
        auto elemTypeName = listSetterElemType(elemType, currentModule, false);
        if (elemTypeName.find(' ') != std::string::npos) {
          out += "Array (";
          out += elemTypeName;
          out += ")";
        } else {
          out += "Array ";
          out += elemTypeName;
        }
      }
      out += ") : Capnp.BuilderM Unit := do\n";
      if (isUnion) {
        out += "  Capnp.setUInt16 b.struct ";
        out += std::to_string(discByteOffset);
        out += " (UInt16.ofNat ";
        out += std::to_string(field.getProto().getDiscriminantValue());
        out += ")\n";
      }
      emitListWrite(elemType,
                    "Capnp.getPointerBuilder b.struct " + std::to_string(offset),
                    "v",
                    currentModule,
                    out,
                    "  ",
                    false);
      out += "\n";
    }

    return out;
  }

  std::string genSetFromValue(Schema schema, kj::StringPtr currentModule, bool recursiveStruct) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    if (recursiveStruct) {
      std::string out;
      out += "def ";
      out += name.cStr();
      out += ".Builder.setFromValue (b : ";
      out += name.cStr();
      out += ".Builder) (v : ";
      out += name.cStr();
      out += ") : Capnp.BuilderM Unit := do\n";
      out += "  Capnp.copyStruct b.struct v.struct\n";
      return out;
    }
    auto structSchema = schema.asStruct();
    auto unionFields = structSchema.getUnionFields();
    auto nonUnionFields = structSchema.getNonUnionFields();

    std::set<std::string> usedAccessors;
    std::unordered_map<uint, std::string> accessorByIndex;
    for (auto field: structSchema.getFields()) {
      auto raw = sanitizeIdentifier(field.getProto().getName());
      auto base = capitalizeIdentifier(raw);
      auto baseName = std::string("get") + base.cStr();
      auto unique = uniqueName(baseName, usedAccessors);
      accessorByIndex[field.getIndex()] = unique;
    }

    std::set<std::string> usedSetters;
    std::set<std::string> usedInits;
    std::unordered_map<uint, std::string> setterByIndex;
    std::unordered_map<uint, std::string> initByIndex;
    for (auto field: structSchema.getFields()) {
      auto raw = sanitizeIdentifier(field.getProto().getName());
      auto base = capitalizeIdentifier(raw);
      auto setName = std::string("set") + base.cStr();
      auto initName = std::string("init") + base.cStr();
      setterByIndex[field.getIndex()] = uniqueName(setName, usedSetters);
      initByIndex[field.getIndex()] = uniqueName(initName, usedInits);
    }

    std::set<std::string> usedFields;
    std::unordered_map<uint, std::string> fieldNameByIndex;
    for (auto field: nonUnionFields) {
      auto rawName = sanitizeIdentifier(field.getProto().getName());
      auto fieldName = uniqueName(rawName.cStr(), usedFields);
      fieldNameByIndex[field.getIndex()] = fieldName;
    }

    std::string whichFieldName;
    if (unionFields.size() > 0) {
      whichFieldName = uniqueName("which", usedFields);
    }

    uint discByteOffset = 0;
    if (unionFields.size() > 0) {
      auto discOffset = structSchema.getProto().getStruct().getDiscriminantOffset();
      discByteOffset = discOffset * 2;
    }

    std::string out;
    out += "def ";
    out += name.cStr();
    out += ".Builder.setFromValue (b : ";
    out += name.cStr();
    out += ".Builder) (v : ";
    out += name.cStr();
    out += ") : Capnp.BuilderM Unit := do\n";

    bool wroteBody = false;

    auto emitFieldSet = [&](StructSchema::Field field,
                            const std::string& valueExpr,
                            const std::string& indent,
                            bool isUnionCase) {
      auto type = field.getType();
      auto itSet = setterByIndex.find(field.getIndex());
      auto setterName = itSet == setterByIndex.end() ? std::string("set") : itSet->second;
      auto itInit = initByIndex.find(field.getIndex());
      auto initName = itInit == initByIndex.end() ? std::string("init") : itInit->second;
      auto itAcc = accessorByIndex.find(field.getIndex());
      auto accessorName = itAcc == accessorByIndex.end() ? std::string("get") : itAcc->second;

      if (field.getProto().isGroup()) {
        if (isUnionCase) {
          out += indent;
          out += "Capnp.setUInt16 b.struct ";
          out += std::to_string(discByteOffset);
          out += " (UInt16.ofNat ";
          out += std::to_string(field.getProto().getDiscriminantValue());
          out += ")\n";
        }
        auto groupTypeId = field.getProto().getGroup().getTypeId();
        auto groupName = qualifiedTypeName(groupTypeId, currentModule);
        out += indent;
        out += "let gb := b.";
        out += accessorName;
        out += "\n";
        out += indent;
        if (isUnionCase) {
          out += "Capnp.copyStruct gb.struct ";
          out += valueExpr;
          out += ".struct\n";
        } else {
          out += groupName.cStr();
          out += ".Builder.setFromValue gb ";
          out += valueExpr;
          out += "\n";
        }
        return;
      }

      auto slot = field.getProto().getSlot();
      auto offset = slot.getOffset();

      switch (type.which()) {
        case schema::Type::VOID:
          if (!valueExpr.empty()) {
            out += indent;
            out += "let _ := ";
            out += valueExpr;
            out += "\n";
          }
          out += indent;
          out += "b.";
          out += setterName;
          out += "\n";
          break;
        case schema::Type::BOOL:
        case schema::Type::INT8:
        case schema::Type::INT16:
        case schema::Type::INT32:
        case schema::Type::INT64:
        case schema::Type::UINT8:
        case schema::Type::UINT16:
        case schema::Type::UINT32:
        case schema::Type::UINT64:
        case schema::Type::FLOAT32:
        case schema::Type::FLOAT64:
        case schema::Type::ENUM:
        case schema::Type::TEXT:
        case schema::Type::DATA:
        case schema::Type::INTERFACE:
          out += indent;
          out += "b.";
          out += setterName;
          out += " ";
          out += valueExpr;
          out += "\n";
          break;
        case schema::Type::STRUCT: {
          auto structName = qualifiedTypeName(type.asStruct().getProto().getId(), currentModule);
          out += indent;
          out += "let sb ← b.";
          out += initName;
          out += "\n";
          out += indent;
          if (isUnionCase) {
            out += "Capnp.copyStruct sb.struct ";
            out += valueExpr;
            out += ".struct\n";
          } else {
            out += structName.cStr();
            out += ".Builder.setFromValue sb ";
            out += valueExpr;
            out += "\n";
          }
          break;
        }
        case schema::Type::LIST: {
          if (isUnionCase) {
            out += indent;
            out += "Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          std::string writeValues = valueExpr;
          if (isUnionCase) {
            writeValues = "Capnp.ListReader.toArray (" + valueExpr + ")";
          }
          emitListWrite(type.asList().getElementType(),
                        "Capnp.getPointerBuilder b.struct " + std::to_string(offset),
                        writeValues,
                        currentModule,
                        out,
                        indent,
                        isUnionCase);
          break;
        }
        case schema::Type::ANY_POINTER: {
          if (isUnionCase) {
            out += indent;
            out += "Capnp.setUInt16 b.struct ";
            out += std::to_string(discByteOffset);
            out += " (UInt16.ofNat ";
            out += std::to_string(field.getProto().getDiscriminantValue());
            out += ")\n";
          }
          out += indent;
          out += "Capnp.copyAnyPointer (Capnp.getPointerBuilder b.struct ";
          out += std::to_string(offset);
          out += ") ";
          out += valueExpr;
          out += "\n";
          break;
        }
        default:
          break;
      }
    };

    for (auto field: nonUnionFields) {
      auto nameIt = fieldNameByIndex.find(field.getIndex());
      auto fieldName = nameIt == fieldNameByIndex.end() ? std::string("field") : nameIt->second;
      emitFieldSet(field, "v." + fieldName, "  ", false);
      wroteBody = true;
    }

    if (unionFields.size() > 0) {
      auto info = buildUnionInfo(structSchema);
      out += "  match v.";
      out += whichFieldName;
      out += " with\n";
      out += "  | ";
      out += name.cStr();
      out += ".Which.";
      out += info.noneName;
      out += " => pure ()\n";
      for (auto& entry: info.cases) {
        auto field = entry.first;
        auto ctor = entry.second;
        out += "  | ";
        out += name.cStr();
        out += ".Which.";
        out += ctor;
        out += " value => do\n";
        emitFieldSet(field, "value", "    ", true);
      }
      wroteBody = true;
    }

    if (!wroteBody) {
      out += "  let _ := b\n";
      out += "  let _ := v\n";
      out += "  pure ()\n";
    }

    return out;
  }

  std::string genInterface(Schema schema, kj::StringPtr currentModule) {
    auto name = qualifiedTypeName(schema.getProto().getId(), currentModule);
    auto interfaceSchema = schema.asInterface();
    auto absoluteTypeName = [&](uint64_t typeId) -> std::string {
      auto qualified = qualifiedTypeName(typeId, currentModule);
      std::string out(qualified.cStr());
      if (out.rfind("Capnp.", 0) == 0) return out;
      return std::string(currentModule.cStr()) + "." + out;
    };
    std::string out;
    out += "abbrev ";
    out += name.cStr();
    out += " := Capnp.Rpc.Client";
    auto nodeAnnOut = genAnnotationUses(name, schema.getProto().getAnnotations(),
                                        currentModule, kj::StringPtr(""));
    if (!nodeAnnOut.empty()) {
      out += "\n\n";
      out += nodeAnnOut;
    }

    out += "\n\nnamespace ";
    out += name.cStr();
    out += "\n\n";
    out += "def interfaceId : UInt64 := UInt64.ofNat ";
    out += std::to_string(schema.getProto().getId());
    out += "\n";

    struct RpcMethodOut {
      std::string fieldName;
      std::string handlerName;
      std::string typedHandlerName;
      std::string advancedTypedHandlerName;
      std::string streamingTypedHandlerName;
      std::string methodIdName;
      std::string methodName;
      std::string callName;
      std::string callMName;
      std::string callWithPayloadRefName;
      std::string callWithPayloadRefMName;
      std::string startCallName;
      std::string startCallMName;
      std::string startCallWithPayloadRefName;
      std::string startCallWithPayloadRefMName;
      std::string awaitCallName;
      std::string awaitPayloadRefCallName;
      std::string responseTypeName;
      std::string promiseTypeName;
      std::string startPromiseName;
      std::string startPromiseMName;
      std::string awaitTypedCallName;
      std::string pipelinedCapName;
      std::string pipelinedCallMName;
      std::string pipelinedTypedCallMName;
      std::string typedCallName;
      std::string typedCallMName;
      std::string encodeRequestName;
      std::string decodeRequestName;
      std::string encodeResponseName;
      std::string decodeResponseName;
      std::string paramTypeName;
      std::string resultTypeName;
    };
    std::vector<RpcMethodOut> rpcMethods;

    std::set<std::string> usedNames;
    usedNames.insert(std::string("interfaceId"));

    for (auto method: interfaceSchema.getMethods()) {
      auto raw = sanitizeIdentifier(method.getProto().getName());
      auto cap = capitalizeIdentifier(raw);
      auto methodBase = std::string(raw.cStr());

      auto fieldName = uniqueName(methodBase, usedNames);
      auto handlerName = uniqueName(methodBase + "Handler", usedNames);
      auto typedHandlerName = uniqueName(methodBase + "TypedHandler", usedNames);
      auto advancedTypedHandlerName = uniqueName(methodBase + "AdvancedTypedHandler", usedNames);
      auto streamingTypedHandlerName = uniqueName(methodBase + "StreamingTypedHandler", usedNames);
      auto methodIdName = uniqueName(methodBase + "MethodId", usedNames);
      auto methodName = uniqueName(methodBase + "Method", usedNames);
      auto callName = uniqueName("call" + std::string(cap.cStr()), usedNames);
      auto callMName = uniqueName("call" + std::string(cap.cStr()) + "M", usedNames);
      auto callWithPayloadRefName =
          uniqueName("call" + std::string(cap.cStr()) + "WithPayloadRef", usedNames);
      auto callWithPayloadRefMName =
          uniqueName("call" + std::string(cap.cStr()) + "WithPayloadRefM", usedNames);
      auto startCallName = uniqueName("start" + std::string(cap.cStr()), usedNames);
      auto startCallMName = uniqueName("start" + std::string(cap.cStr()) + "M", usedNames);
      auto startCallWithPayloadRefName =
          uniqueName("start" + std::string(cap.cStr()) + "WithPayloadRef", usedNames);
      auto startCallWithPayloadRefMName =
          uniqueName("start" + std::string(cap.cStr()) + "WithPayloadRefM", usedNames);
      auto awaitCallName = uniqueName("await" + std::string(cap.cStr()), usedNames);
      auto awaitPayloadRefCallName =
          uniqueName("await" + std::string(cap.cStr()) + "PayloadRef", usedNames);
      auto responseTypeName = uniqueName(std::string(cap.cStr()) + "Response", usedNames);
      auto promiseTypeName = uniqueName(std::string(cap.cStr()) + "Promise", usedNames);
      auto startPromiseName = uniqueName("start" + std::string(cap.cStr()) + "Promise", usedNames);
      auto startPromiseMName = uniqueName("start" + std::string(cap.cStr()) + "PromiseM", usedNames);
      auto awaitTypedCallName = uniqueName("await" + std::string(cap.cStr()) + "Typed", usedNames);
      auto pipelinedCapName = uniqueName("get" + std::string(cap.cStr()) + "PipelinedCap", usedNames);
      auto pipelinedCallMName = uniqueName("call" + std::string(cap.cStr()) + "PipelinedM", usedNames);
      auto pipelinedTypedCallMName = uniqueName("call" + std::string(cap.cStr()) + "PipelinedTypedM", usedNames);
      auto typedCallName = uniqueName("call" + std::string(cap.cStr()) + "Typed", usedNames);
      auto typedCallMName = uniqueName("call" + std::string(cap.cStr()) + "TypedM", usedNames);
      auto encodeRequestName = uniqueName(methodBase + "RequestToPayload", usedNames);
      auto decodeRequestName = uniqueName(methodBase + "RequestOfPayload", usedNames);
      auto encodeResponseName = uniqueName(methodBase + "ResponseToPayload", usedNames);
      auto decodeResponseName = uniqueName(methodBase + "ResponseOfPayload", usedNames);
      auto paramTypeName = absoluteTypeName(method.getParamType().getProto().getId());
      auto resultTypeName = absoluteTypeName(method.getResultType().getProto().getId());
      rpcMethods.push_back({fieldName, handlerName, typedHandlerName, advancedTypedHandlerName,
                            streamingTypedHandlerName, methodIdName, methodName, callName,
                            callMName, callWithPayloadRefName, callWithPayloadRefMName,
                            startCallName, startCallMName, startCallWithPayloadRefName,
                            startCallWithPayloadRefMName, awaitCallName,
                            awaitPayloadRefCallName, responseTypeName, promiseTypeName,
                            startPromiseName, startPromiseMName, awaitTypedCallName,
                            pipelinedCapName, pipelinedCallMName, pipelinedTypedCallMName,
                            typedCallName, typedCallMName, encodeRequestName, decodeRequestName,
                            encodeResponseName, decodeResponseName, paramTypeName, resultTypeName});

      out += "\n";
      out += "def ";
      out += methodIdName;
      out += " : UInt16 := UInt16.ofNat ";
      out += std::to_string(method.getOrdinal());
      out += "\n";

      out += "def ";
      out += methodName;
      out += " : Capnp.Rpc.Method := { interfaceId := interfaceId, methodId := ";
      out += methodIdName;
      out += " }\n";

      out += "def ";
      out += callName;
      out += " (backend : Capnp.Rpc.Backend) (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : IO Capnp.Rpc.Payload := do\n";
      out += "  Capnp.Rpc.call backend target ";
      out += methodName;
      out += " payload\n";

      out += "def ";
      out += callMName;
      out += " (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM Capnp.Rpc.Payload := do\n";
      out += "  Capnp.Rpc.RuntimeM.call target ";
      out += methodName;
      out += " payload\n";

      out += "def ";
      out += callWithPayloadRefName;
      out += " (runtime : Capnp.Rpc.Runtime) (target : ";
      out += name.cStr();
      out += ") (payloadRef : Capnp.Rpc.RuntimePayloadRef) : IO Capnp.Rpc.RuntimePayloadRef := do\n";
      out += "  Capnp.Rpc.Runtime.callWithPayloadRef runtime target ";
      out += methodName;
      out += " payloadRef\n";

      out += "def ";
      out += callWithPayloadRefMName;
      out += " (target : ";
      out += name.cStr();
      out += ") (payloadRef : Capnp.Rpc.RuntimePayloadRef) : Capnp.Rpc.RuntimeM Capnp.Rpc.RuntimePayloadRef := do\n";
      out += "  Capnp.Rpc.RuntimeM.callWithPayloadRef target ";
      out += methodName;
      out += " payloadRef\n";

      out += "def ";
      out += startCallName;
      out += " (runtime : Capnp.Rpc.Runtime) (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : IO Capnp.Rpc.RuntimePendingCallRef := do\n";
      out += "  Capnp.Rpc.Runtime.startCall runtime target ";
      out += methodName;
      out += " payload\n";

      out += "def ";
      out += startCallMName;
      out += " (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM Capnp.Rpc.RuntimePendingCallRef := do\n";
      out += "  Capnp.Rpc.RuntimeM.startCall target ";
      out += methodName;
      out += " payload\n";

      out += "def ";
      out += startCallWithPayloadRefName;
      out += " (runtime : Capnp.Rpc.Runtime) (target : ";
      out += name.cStr();
      out += ") (payloadRef : Capnp.Rpc.RuntimePayloadRef) : IO Capnp.Rpc.RuntimePendingCallRef := do\n";
      out += "  Capnp.Rpc.Runtime.startCallWithPayloadRef runtime target ";
      out += methodName;
      out += " payloadRef\n";

      out += "def ";
      out += startCallWithPayloadRefMName;
      out += " (target : ";
      out += name.cStr();
      out += ") (payloadRef : Capnp.Rpc.RuntimePayloadRef) : Capnp.Rpc.RuntimeM Capnp.Rpc.RuntimePendingCallRef := do\n";
      out += "  Capnp.Rpc.RuntimeM.startCallWithPayloadRef target ";
      out += methodName;
      out += " payloadRef\n";

      out += "def ";
      out += awaitCallName;
      out += " (pendingCall : Capnp.Rpc.RuntimePendingCallRef) : IO Capnp.Rpc.Payload := do\n";
      out += "  pendingCall.await\n";

      out += "def ";
      out += awaitPayloadRefCallName;
      out += " (pendingCall : Capnp.Rpc.RuntimePendingCallRef) : IO Capnp.Rpc.RuntimePayloadRef := do\n";
      out += "  pendingCall.awaitPayloadRef\n";

      auto paramReaderTypeName = paramTypeName + ".Reader";
      auto resultReaderTypeName = resultTypeName + ".Reader";

      out += "abbrev ";
      out += responseTypeName;
      out += " := Capnp.Rpc.TypedPayload ";
      out += resultReaderTypeName;
      out += "\n";

      out += "abbrev ";
      out += promiseTypeName;
      out += " := Capnp.Rpc.Promise ";
      out += responseTypeName;
      out += "\n";

      out += "def ";
      out += startPromiseName;
      out += " (runtime : Capnp.Rpc.Runtime) (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : IO ";
      out += promiseTypeName;
      out += " := do\n";
      out += "  return { pendingCall := (← ";
      out += startCallName;
      out += " runtime target payload) }\n";

      out += "def ";
      out += startPromiseMName;
      out += " (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM ";
      out += promiseTypeName;
      out += " := do\n";
      out += "  return { pendingCall := (← ";
      out += startCallMName;
      out += " target payload) }\n";

      out += "def ";
      out += promiseTypeName;
      out += ".await (promise : ";
      out += promiseTypeName;
      out += ") : IO Capnp.Rpc.Payload := do\n";
      out += "  ";
      out += awaitCallName;
      out += " promise.pendingCall\n";

      out += "def ";
      out += promiseTypeName;
      out += ".awaitPayloadRef (promise : ";
      out += promiseTypeName;
      out += ") : IO Capnp.Rpc.RuntimePayloadRef := do\n";
      out += "  ";
      out += awaitPayloadRefCallName;
      out += " promise.pendingCall\n";

      out += "def ";
      out += promiseTypeName;
      out += ".release (promise : ";
      out += promiseTypeName;
      out += ") : IO Unit :=\n";
      out += "  promise.pendingCall.release\n";

      out += "def ";
      out += promiseTypeName;
      out += ".releaseDeferred (promise : ";
      out += promiseTypeName;
      out += ") : IO Unit :=\n";
      out += "  promise.pendingCall.releaseDeferred\n";

      out += "abbrev ";
      out += typedHandlerName;
      out += " := Capnp.Rpc.Client -> ";
      out += paramReaderTypeName;
      out += " -> Capnp.CapTable -> IO Capnp.Rpc.Payload\n";

      out += "abbrev ";
      out += advancedTypedHandlerName;
      out += " := Capnp.Rpc.Client -> ";
      out += paramReaderTypeName;
      out += " -> Capnp.CapTable -> IO Capnp.Rpc.AdvancedHandlerReply\n";

      out += "abbrev ";
      out += streamingTypedHandlerName;
      out += " := Capnp.Rpc.Client -> ";
      out += paramReaderTypeName;
      out += " -> Capnp.CapTable -> IO Capnp.Rpc.AdvancedHandlerReply\n";

      out += "def ";
      out += decodeRequestName;
      out += " (payload : Capnp.Rpc.Payload) : IO (";
      out += paramReaderTypeName;
      out += " × Capnp.CapTable) := do\n";
      out += "  let reader ← match ";
      out += paramTypeName;
      out += ".readChecked (Capnp.getRoot payload.msg) with\n";
      out += "    | Except.ok r => pure r\n";
      out += "    | Except.error e => throw (IO.userError s!\"invalid ";
      out += methodName;
      out += " request: {e}\")\n";
      out += "  return (reader, payload.capTable)\n";

      out += "def ";
      out += decodeResponseName;
      out += " (payload : Capnp.Rpc.Payload) : IO ";
      out += responseTypeName;
      out += " := do\n";
      out += "  let reader ← match ";
      out += resultTypeName;
      out += ".readChecked (Capnp.getRoot payload.msg) with\n";
      out += "    | Except.ok r => pure r\n";
      out += "    | Except.error e => throw (IO.userError s!\"invalid ";
      out += methodName;
      out += " response: {e}\")\n";
      out += "  return { reader := reader, capTable := payload.capTable }\n";

      out += "def ";
      out += typedCallName;
      out += " (backend : Capnp.Rpc.Backend) (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : IO ";
      out += responseTypeName;
      out += " := do\n";
      out += "  let response ← Capnp.Rpc.call backend target ";
      out += methodName;
      out += " payload\n";
      out += "  ";
      out += decodeResponseName;
      out += " response\n";

      out += "def ";
      out += typedCallMName;
      out += " (target : ";
      out += name.cStr();
      out += ") (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM ";
      out += responseTypeName;
      out += " := do\n";
      out += "  let response ← Capnp.Rpc.RuntimeM.call target ";
      out += methodName;
      out += " payload\n";
      out += "  ";
      out += decodeResponseName;
      out += " response\n";

      out += "def ";
      out += awaitTypedCallName;
      out += " (pendingCall : Capnp.Rpc.RuntimePendingCallRef) : IO ";
      out += responseTypeName;
      out += " := do\n";
      out += "  let response ← pendingCall.await\n";
      out += "  ";
      out += decodeResponseName;
      out += " response\n";

      out += "def ";
      out += promiseTypeName;
      out += ".awaitTyped (promise : ";
      out += promiseTypeName;
      out += ") : IO ";
      out += responseTypeName;
      out += " := do\n";
      out += "  ";
      out += awaitTypedCallName;
      out += " promise.pendingCall\n";

      out += "def ";
      out += pipelinedCapName;
      out += " (pendingCall : Capnp.Rpc.RuntimePendingCallRef)\n";
      out += "    (pointerPath : Array UInt16 := #[]) : IO ";
      out += name.cStr();
      out += " := do\n";
      out += "  pendingCall.getPipelinedCap pointerPath\n";

      out += "def ";
      out += pipelinedCallMName;
      out += " (pendingCall : Capnp.Rpc.RuntimePendingCallRef)\n";
      out += "    (pointerPath : Array UInt16 := #[])\n";
      out += "    (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM Capnp.Rpc.Payload := do\n";
      out += "  let target ← Capnp.Rpc.RuntimeM.pendingCallGetPipelinedCap pendingCall pointerPath\n";
      out += "  Capnp.Rpc.RuntimeM.call target ";
      out += methodName;
      out += " payload\n";

      out += "def ";
      out += pipelinedTypedCallMName;
      out += " (pendingCall : Capnp.Rpc.RuntimePendingCallRef)\n";
      out += "    (pointerPath : Array UInt16 := #[])\n";
      out += "    (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM ";
      out += responseTypeName;
      out += " := do\n";
      out += "  let response ← ";
      out += pipelinedCallMName;
      out += " pendingCall pointerPath payload\n";
      out += "  ";
      out += decodeResponseName;
      out += " response\n";

      out += "def ";
      out += promiseTypeName;
      out += ".getPipelinedCap (promise : ";
      out += promiseTypeName;
      out += ")\n";
      out += "    (pointerPath : Array UInt16 := #[]) : IO ";
      out += name.cStr();
      out += " := do\n";
      out += "  ";
      out += pipelinedCapName;
      out += " promise.pendingCall pointerPath\n";

      out += "def ";
      out += promiseTypeName;
      out += ".callPipelinedM (promise : ";
      out += promiseTypeName;
      out += ")\n";
      out += "    (pointerPath : Array UInt16 := #[])\n";
      out += "    (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM Capnp.Rpc.Payload := do\n";
      out += "  ";
      out += pipelinedCallMName;
      out += " promise.pendingCall pointerPath payload\n";

      out += "def ";
      out += promiseTypeName;
      out += ".callPipelinedTypedM (promise : ";
      out += promiseTypeName;
      out += ")\n";
      out += "    (pointerPath : Array UInt16 := #[])\n";
      out += "    (payload : Capnp.Rpc.Payload := Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM ";
      out += responseTypeName;
      out += " := do\n";
      out += "  ";
      out += pipelinedTypedCallMName;
      out += " promise.pendingCall pointerPath payload\n";

      auto methodOwner = std::string(name.cStr()) + "." + methodName;
      auto methodAnnOut = genAnnotationUses(
          kj::StringPtr(methodOwner.c_str(), methodOwner.size()),
          method.getProto().getAnnotations(),
          currentModule,
          kj::StringPtr(""));
      if (!methodAnnOut.empty()) {
        out += "\n";
        out += methodAnnOut;
      }
    }

    auto restoreSturdyRefName = uniqueName("restoreSturdyRef", usedNames);
    auto restoreSturdyRefMName = uniqueName("restoreSturdyRefM", usedNames);
    auto restoreSturdyRefStartName = uniqueName("restoreSturdyRefStart", usedNames);
    auto restoreSturdyRefStartMName = uniqueName("restoreSturdyRefStartM", usedNames);
    auto awaitRestoreSturdyRefName = uniqueName("awaitRestoreSturdyRef", usedNames);
    auto restoreSturdyRefAsTaskName = uniqueName("restoreSturdyRefAsTask", usedNames);
    auto restoreSturdyRefAsTaskMName = uniqueName("restoreSturdyRefAsTaskM", usedNames);
    auto restoreSturdyRefAsPromiseName = uniqueName("restoreSturdyRefAsPromise", usedNames);
    auto restoreSturdyRefAsPromiseMName = uniqueName("restoreSturdyRefAsPromiseM", usedNames);
    auto newPromiseCapabilityName = uniqueName("newPromiseCapability", usedNames);
    auto newPromiseCapabilityMName = uniqueName("newPromiseCapabilityM", usedNames);
    auto promiseCapabilityFulfillName = uniqueName("promiseCapabilityFulfill", usedNames);
    auto promiseCapabilityFulfillMName = uniqueName("promiseCapabilityFulfillM", usedNames);
    auto promiseCapabilityRejectName = uniqueName("promiseCapabilityReject", usedNames);
    auto promiseCapabilityRejectMName = uniqueName("promiseCapabilityRejectM", usedNames);
    auto promiseCapabilityReleaseName = uniqueName("promiseCapabilityRelease", usedNames);
    auto promiseCapabilityReleaseMName = uniqueName("promiseCapabilityReleaseM", usedNames);

    out += "\n";
    out += "def ";
    out += restoreSturdyRefName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : IO ";
    out += name.cStr();
    out += " := do\n";
    out += "  peer.restoreSturdyRef sturdyRef\n";

    out += "def ";
    out += restoreSturdyRefMName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : Capnp.Rpc.RuntimeM ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRef peer sturdyRef\n";

    out += "def ";
    out += restoreSturdyRefStartName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : IO Capnp.Rpc.RuntimeRegisterPromiseRef := do\n";
    out += "  peer.restoreSturdyRefStart sturdyRef\n";

    out += "def ";
    out += restoreSturdyRefStartMName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : Capnp.Rpc.RuntimeM Capnp.Rpc.RuntimeRegisterPromiseRef := do\n";
    out += "  Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefStart peer sturdyRef\n";

    out += "def ";
    out += awaitRestoreSturdyRefName;
    out += " (promise : Capnp.Rpc.RuntimeRegisterPromiseRef) : IO ";
    out += name.cStr();
    out += " := do\n";
    out += "  promise.awaitTarget\n";

    out += "def ";
    out += restoreSturdyRefAsTaskName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : IO (Task (Except IO.Error ";
    out += name.cStr();
    out += ")) := do\n";
    out += "  peer.restoreSturdyRefAsTask sturdyRef\n";

    out += "def ";
    out += restoreSturdyRefAsTaskMName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : Capnp.Rpc.RuntimeM (Task (Except IO.Error ";
    out += name.cStr();
    out += ")) := do\n";
    out += "  Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefAsTask peer sturdyRef\n";

    out += "def ";
    out += restoreSturdyRefAsPromiseName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : IO (Capnp.Async.Promise ";
    out += name.cStr();
    out += ") := do\n";
    out += "  peer.restoreSturdyRefAsPromise sturdyRef\n";

    out += "def ";
    out += restoreSturdyRefAsPromiseMName;
    out += " (peer : Capnp.Rpc.RuntimeVatPeerRef) (sturdyRef : Capnp.Rpc.SturdyRef) : Capnp.Rpc.RuntimeM (Capnp.Async.Promise ";
    out += name.cStr();
    out += ") := do\n";
    out += "  Capnp.Rpc.RuntimeM.multiVatRestoreSturdyRefAsPromise peer sturdyRef\n";

    out += "\n";
    out += "def ";
    out += newPromiseCapabilityName;
    out += " (runtime : Capnp.Rpc.Runtime) : IO (";
    out += name.cStr();
    out += " × Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef) := do\n";
    out += "  runtime.newPromiseCapability\n";

    out += "def ";
    out += newPromiseCapabilityMName;
    out += " : Capnp.Rpc.RuntimeM (";
    out += name.cStr();
    out += " × Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef) := do\n";
    out += "  Capnp.Rpc.RuntimeM.newPromiseCapability\n";

    out += "def ";
    out += promiseCapabilityFulfillName;
    out += " (fulfiller : Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef) (target : ";
    out += name.cStr();
    out += ") : IO Unit := do\n";
    out += "  fulfiller.fulfill target\n";

    out += "def ";
    out += promiseCapabilityFulfillMName;
    out += " (fulfiller : Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef) (target : ";
    out += name.cStr();
    out += ") : Capnp.Rpc.RuntimeM Unit := do\n";
    out += "  Capnp.Rpc.RuntimeM.promiseCapabilityFulfill fulfiller target\n";

    out += "def ";
    out += promiseCapabilityRejectName;
    out += " (fulfiller : Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef)\n";
    out += "    (type : Capnp.Rpc.RemoteExceptionType)\n";
    out += "    (message : String := \"\")\n";
    out += "    (detail : ByteArray := ByteArray.empty) : IO Unit := do\n";
    out += "  fulfiller.reject type message detail\n";

    out += "def ";
    out += promiseCapabilityRejectMName;
    out += " (fulfiller : Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef)\n";
    out += "    (type : Capnp.Rpc.RemoteExceptionType)\n";
    out += "    (message : String := \"\")\n";
    out += "    (detail : ByteArray := ByteArray.empty) : Capnp.Rpc.RuntimeM Unit := do\n";
    out += "  Capnp.Rpc.RuntimeM.promiseCapabilityReject fulfiller type message detail\n";

    out += "def ";
    out += promiseCapabilityReleaseName;
    out += " (fulfiller : Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef) : IO Unit := do\n";
    out += "  fulfiller.release\n";

    out += "def ";
    out += promiseCapabilityReleaseMName;
    out += " (fulfiller : Capnp.Rpc.RuntimePromiseCapabilityFulfillerRef) : Capnp.Rpc.RuntimeM Unit := do\n";
    out += "  Capnp.Rpc.RuntimeM.promiseCapabilityRelease fulfiller\n";

    auto handlerTypeName = uniqueName("Handler", usedNames);
    auto serverTypeName = uniqueName("Server", usedNames);
    auto dispatchName = uniqueName("dispatch", usedNames);
    auto backendName = uniqueName("backend", usedNames);
    auto registerTargetName = uniqueName("registerTarget", usedNames);
    auto registerTargetMName = uniqueName("registerTargetM", usedNames);
    auto typedServerTypeName = uniqueName("TypedServer", usedNames);
    auto typedDispatchName = uniqueName("typedDispatch", usedNames);
    auto typedBackendName = uniqueName("typedBackend", usedNames);
    auto registerTypedTargetName = uniqueName("registerTypedTarget", usedNames);
    auto registerTypedTargetMName = uniqueName("registerTypedTargetM", usedNames);
    auto advancedTypedServerTypeName = uniqueName("AdvancedTypedServer", usedNames);
    auto advancedTypedTargetHandlerName = uniqueName("advancedTypedTargetHandler", usedNames);
    auto registerAdvancedTypedTargetName = uniqueName("registerAdvancedTypedTarget", usedNames);
    auto registerAdvancedTypedTargetMName = uniqueName("registerAdvancedTypedTargetM", usedNames);
    auto streamingTypedServerTypeName = uniqueName("StreamingTypedServer", usedNames);
    auto streamingTypedTargetHandlerName = uniqueName("streamingTypedTargetHandler", usedNames);
    auto registerStreamingTypedTargetName = uniqueName("registerStreamingTypedTarget", usedNames);
    auto registerStreamingTypedTargetMName = uniqueName("registerStreamingTypedTargetM", usedNames);

    out += "\n";
    out += "abbrev ";
    out += handlerTypeName;
    out += " := Capnp.Rpc.Handler\n";

    for (auto& methodOut: rpcMethods) {
      out += "abbrev ";
      out += methodOut.handlerName;
      out += " := ";
      out += handlerTypeName;
      out += "\n";
    }

    out += "\n";
    out += "structure ";
    out += serverTypeName;
    out += " where\n";
    if (rpcMethods.empty()) {
      out += "  unit : Unit := ()\n";
    } else {
      for (auto& methodOut: rpcMethods) {
        out += "  ";
        out += methodOut.fieldName;
        out += " : ";
        out += methodOut.handlerName;
        out += "\n";
      }
    }

    out += "\n";
    out += "def ";
    out += dispatchName;
    out += " (server : ";
    out += serverTypeName;
    out += ") : Capnp.Rpc.Dispatch := Id.run do\n";
    out += "  let mut d := Capnp.Rpc.Dispatch.empty\n";
    if (rpcMethods.empty()) {
      out += "  let _ := server\n";
    }
    for (auto& methodOut: rpcMethods) {
      out += "  d := Capnp.Rpc.Dispatch.register d ";
      out += methodOut.methodName;
      out += " server.";
      out += methodOut.fieldName;
      out += "\n";
    }
    out += "  return d\n";

    out += "\n";
    out += "def ";
    out += backendName;
    out += " (server : ";
    out += serverTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) : Capnp.Rpc.Backend where\n";
    out += "  call := fun target method payload => do\n";
    if (rpcMethods.empty()) {
      out += "    let _ := server\n";
      out += "    onMissing target method payload\n";
    } else {
      bool first = true;
      for (auto& methodOut: rpcMethods) {
        out += first ? "    if method == " : "    else if method == ";
        out += methodOut.methodName;
        out += " then\n";
        out += "      server.";
        out += methodOut.fieldName;
        out += " target payload\n";
        first = false;
      }
      out += "    else\n";
      out += "      onMissing target method payload\n";
    }

    out += "\n";
    out += "def ";
    out += registerTargetName;
    out += " (runtime : Capnp.Rpc.Runtime) (server : ";
    out += serverTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) : IO ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.Runtime.registerBackendTarget runtime (";
    out += backendName;
    out += " server (onMissing := onMissing))\n";

    out += "\n";
    out += "def ";
    out += registerTargetMName;
    out += " (server : ";
    out += serverTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.RuntimeM.registerBackendTarget (";
    out += backendName;
    out += " server (onMissing := onMissing))\n";

    out += "\n";
    out += "structure ";
    out += typedServerTypeName;
    out += " where\n";
    if (rpcMethods.empty()) {
      out += "  unit : Unit := ()\n";
    } else {
      for (auto& methodOut: rpcMethods) {
        out += "  ";
        out += methodOut.fieldName;
        out += " : ";
        out += methodOut.typedHandlerName;
        out += "\n";
      }
    }

    out += "\n";
    out += "def ";
    out += typedDispatchName;
    out += " (server : ";
    out += typedServerTypeName;
    out += ") : Capnp.Rpc.Dispatch := Id.run do\n";
    out += "  let mut d := Capnp.Rpc.Dispatch.empty\n";
    if (rpcMethods.empty()) {
      out += "  let _ := server\n";
    }
    for (auto& methodOut: rpcMethods) {
      out += "  d := Capnp.Rpc.Dispatch.register d ";
      out += methodOut.methodName;
      out += " (fun target payload => do\n";
      out += "    let (request, requestCaps) ← ";
      out += methodOut.decodeRequestName;
      out += " payload\n";
      out += "    server.";
      out += methodOut.fieldName;
      out += " target request requestCaps\n";
      out += "  )\n";
    }
    out += "  return d\n";

    out += "\n";
    out += "def ";
    out += typedBackendName;
    out += " (server : ";
    out += typedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) : Capnp.Rpc.Backend where\n";
    out += "  call := fun target method payload => do\n";
    if (rpcMethods.empty()) {
      out += "    let _ := server\n";
      out += "    onMissing target method payload\n";
    } else {
      bool first = true;
      for (auto& methodOut: rpcMethods) {
        out += first ? "    if method == " : "    else if method == ";
        out += methodOut.methodName;
        out += " then do\n";
        out += "      let (request, requestCaps) ← ";
        out += methodOut.decodeRequestName;
        out += " payload\n";
        out += "      server.";
        out += methodOut.fieldName;
        out += " target request requestCaps\n";
        first = false;
      }
      out += "    else\n";
      out += "      onMissing target method payload\n";
    }

    out += "\n";
    out += "def ";
    out += registerTypedTargetName;
    out += " (runtime : Capnp.Rpc.Runtime) (server : ";
    out += typedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) : IO ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.Runtime.registerBackendTarget runtime (";
    out += typedBackendName;
    out += " server (onMissing := onMissing))\n";

    out += "\n";
    out += "def ";
    out += registerTypedTargetMName;
    out += " (server : ";
    out += typedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.Payload := fun _ _ _ => pure Capnp.emptyRpcEnvelope) : Capnp.Rpc.RuntimeM ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.RuntimeM.registerBackendTarget (";
    out += typedBackendName;
    out += " server (onMissing := onMissing))\n";

    out += "\n";
    out += "structure ";
    out += advancedTypedServerTypeName;
    out += " where\n";
    if (rpcMethods.empty()) {
      out += "  unit : Unit := ()\n";
    } else {
      for (auto& methodOut: rpcMethods) {
        out += "  ";
        out += methodOut.fieldName;
        out += " : ";
        out += methodOut.advancedTypedHandlerName;
        out += "\n";
      }
    }

    out += "\n";
    out += "def ";
    out += advancedTypedTargetHandlerName;
    out += " (server : ";
    out += advancedTypedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply := fun _ _ _ => pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))) : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply :=\n";
    out += "  fun target method payload => do\n";
    if (rpcMethods.empty()) {
      out += "    let _ := server\n";
      out += "    onMissing target method payload\n";
    } else {
      bool first = true;
      for (auto& methodOut: rpcMethods) {
        out += first ? "    if method == " : "    else if method == ";
        out += methodOut.methodName;
        out += " then do\n";
        out += "      let (request, requestCaps) ← ";
        out += methodOut.decodeRequestName;
        out += " payload\n";
        out += "      server.";
        out += methodOut.fieldName;
        out += " target request requestCaps\n";
        first = false;
      }
      out += "    else\n";
      out += "      onMissing target method payload\n";
    }

    out += "\n";
    out += "def ";
    out += registerAdvancedTypedTargetName;
    out += " (runtime : Capnp.Rpc.Runtime) (server : ";
    out += advancedTypedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply := fun _ _ _ => pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))) : IO ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.Runtime.registerAdvancedHandlerTargetAsync runtime (";
    out += advancedTypedTargetHandlerName;
    out += " server (onMissing := onMissing))\n";

    out += "\n";
    out += "def ";
    out += registerAdvancedTypedTargetMName;
    out += " (server : ";
    out += advancedTypedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply := fun _ _ _ => pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))) : Capnp.Rpc.RuntimeM ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.RuntimeM.registerAdvancedHandlerTargetAsync (";
    out += advancedTypedTargetHandlerName;
    out += " server (onMissing := onMissing))\n";

    out += "\n";
    out += "structure ";
    out += streamingTypedServerTypeName;
    out += " where\n";
    if (rpcMethods.empty()) {
      out += "  unit : Unit := ()\n";
    } else {
      for (auto& methodOut: rpcMethods) {
        out += "  ";
        out += methodOut.fieldName;
        out += " : ";
        out += methodOut.streamingTypedHandlerName;
        out += "\n";
      }
    }

    out += "\n";
    out += "def ";
    out += streamingTypedTargetHandlerName;
    out += " (server : ";
    out += streamingTypedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply := fun _ _ _ => pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))) : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply :=\n";
    out += "  fun target method payload => do\n";
    if (rpcMethods.empty()) {
      out += "    let _ := server\n";
      out += "    onMissing target method payload\n";
    } else {
      bool first = true;
      for (auto& methodOut: rpcMethods) {
        out += first ? "    if method == " : "    else if method == ";
        out += methodOut.methodName;
        out += " then do\n";
        out += "      let (request, requestCaps) ← ";
        out += methodOut.decodeRequestName;
        out += " payload\n";
        out += "      server.";
        out += methodOut.fieldName;
        out += " target request requestCaps\n";
        first = false;
      }
      out += "    else\n";
      out += "      onMissing target method payload\n";
    }

    out += "\n";
    out += "def ";
    out += registerStreamingTypedTargetName;
    out += " (runtime : Capnp.Rpc.Runtime) (server : ";
    out += streamingTypedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply := fun _ _ _ => pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))) : IO ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.Runtime.registerStreamingHandlerTargetAsync runtime (";
    out += streamingTypedTargetHandlerName;
    out += " server (onMissing := onMissing))\n";

    out += "\n";
    out += "def ";
    out += registerStreamingTypedTargetMName;
    out += " (server : ";
    out += streamingTypedServerTypeName;
    out += ")\n";
    out += "    (onMissing : Capnp.Rpc.Client -> Capnp.Rpc.Method -> Capnp.Rpc.Payload -> IO Capnp.Rpc.AdvancedHandlerReply := fun _ _ _ => pure (Capnp.Rpc.Advanced.now (Capnp.Rpc.Advanced.respond Capnp.emptyRpcEnvelope))) : Capnp.Rpc.RuntimeM ";
    out += name.cStr();
    out += " := do\n";
    out += "  Capnp.Rpc.RuntimeM.registerStreamingHandlerTargetAsync (";
    out += streamingTypedTargetHandlerName;
    out += " server (onMissing := onMissing))\n";

    out += "\nend ";
    out += name.cStr();
    return out;
  }

  std::string genConst(Schema schema, kj::StringPtr currentModule) {
    auto name = sanitizeIdentifier(getUnqualifiedName(schema));
    auto constSchema = schema.asConst();
    std::string out;
    auto value = schema.getProto().getConst().getValue();
    auto valueExpr = valueToLean(constSchema.getType(), value, currentModule);
    if (valueExpr.empty()) {
      out += "opaque ";
      out += name.cStr();
      out += " : ";
      out += typeToLean(constSchema.getType(), currentModule);
    } else {
      out += "def ";
      out += name.cStr();
    out += " : ";
    out += typeToLean(constSchema.getType(), currentModule);
    out += " := ";
      out += valueExpr;
    }

    auto annOut = genAnnotationUses(name, schema.getProto().getAnnotations(),
                                    currentModule, kj::StringPtr(""));
    if (!annOut.empty()) {
      out += "\n";
      out += annOut;
    }
    return out;
  }

  std::string genConstNodes(Schema schema, kj::StringPtr currentModule) {
    auto proto = schema.getProto();
    std::string out;
    if (proto.which() == schema::Node::CONST) {
      out += genConst(schema, currentModule);
    }
    for (auto nested: proto.getNestedNodes()) {
      auto nestedSchema = schemaLoader.get(nested.getId());
      auto nestedOut = genConstNodes(nestedSchema, currentModule);
      if (!nestedOut.empty()) {
        if (!out.empty()) out += "\n";
        out += nestedOut;
      }
    }
    return out;
  }

  std::string genNonStructNodes(Schema schema, kj::StringPtr currentModule) {
    auto proto = schema.getProto();
    std::string out;
    switch (proto.which()) {
      case schema::Node::FILE:
      case schema::Node::STRUCT:
        break;
      case schema::Node::ENUM:
        out += genEnum(schema, currentModule);
        break;
      case schema::Node::INTERFACE:
        out += genInterface(schema, currentModule);
        break;
      case schema::Node::ANNOTATION:
        out += genAnnotation(schema, currentModule);
        break;
      default:
        break;
    }
    for (auto nested: proto.getNestedNodes()) {
      auto nestedSchema = schemaLoader.get(nested.getId());
      auto nestedOut = genNonStructNodes(nestedSchema, currentModule);
      if (!nestedOut.empty()) {
        if (!out.empty()) out += "\n";
        out += nestedOut;
      }
    }
    return out;
  }

  std::string genAnnotation(Schema schema, kj::StringPtr currentModule) {
    auto name = sanitizeIdentifier(getUnqualifiedName(schema));
    auto annotation = schema.getProto().getAnnotation();
    std::string out;
    out += "opaque ";
    out += name.cStr();
    out += " : ";
    out += typeToLeanProto(annotation.getType(), currentModule);
    return out;
  }

  std::string genAnnotationUses(kj::StringPtr ownerName,
                                AnnotationList annotations,
                                kj::StringPtr currentModule,
                                kj::StringPtr prefix) {
    if (annotations.size() == 0) return "";
    std::set<std::string> used;
    std::string out;
    for (auto ann: annotations) {
      auto annSchema = schemaLoader.get(ann.getId());
      auto annName = sanitizeIdentifier(getUnqualifiedName(annSchema));
      std::string base = "_ann";
      if (prefix.size() > 0) {
        base += "_";
        base += prefix.cStr();
      }
      base += "_";
      base += annName.cStr();
      auto defName = uniqueName(base, used);
      auto annType = annSchema.getProto().getAnnotation().getType();
      out += "def ";
      out += ownerName.cStr();
      out += ".";
      out += defName;
      out += " : ";
      out += typeToLeanProto(annType, currentModule);
      out += " := ";
      auto valueExpr = valueToLeanProto(annType, ann.getValue(), currentModule);
      KJ_REQUIRE(!valueExpr.empty(),
          "Lean4 backend cannot serialize this annotation value.",
          ownerName, defName.c_str());
      out += valueExpr;
      out += "\n";
    }
    return out;
  }

  std::string genNode(Schema schema, kj::StringPtr currentModule) {
    auto proto = schema.getProto();
    std::string out;
    if (proto.which() == schema::Node::STRUCT) {
      std::unordered_set<uint64_t> nestedIds;
      for (auto nested: proto.getNestedNodes()) {
        nestedIds.insert(nested.getId());
      }

      auto emitNestedKind = [&](schema::Node::Which kind) {
        for (auto nested: proto.getNestedNodes()) {
          auto nestedSchema = schemaLoader.get(nested.getId());
          if (nestedSchema.getProto().which() != kind) continue;
          out += "\n";
          out += genNode(nestedSchema, currentModule);
        }
      };
      emitNestedKind(schema::Node::ENUM);
      emitNestedKind(schema::Node::INTERFACE);
      emitNestedKind(schema::Node::ANNOTATION);
      emitNestedKind(schema::Node::STRUCT);

      auto structSchema = schema.asStruct();
      for (auto field: structSchema.getFields()) {
        if (!field.getProto().isGroup()) continue;
        auto groupId = field.getProto().getGroup().getTypeId();
        if (nestedIds.insert(groupId).second) {
          out += "\n";
          out += genNode(schemaLoader.get(groupId), currentModule);
        }
      }

      out += "\n";
      out += genStruct(schema, currentModule);
      return out;
    }

    switch (proto.which()) {
      case schema::Node::ENUM:
        out += genEnum(schema, currentModule);
        break;
      case schema::Node::INTERFACE:
        out += genInterface(schema, currentModule);
        break;
      case schema::Node::CONST:
        break;
      case schema::Node::ANNOTATION:
        out += genAnnotation(schema, currentModule);
        break;
      default:
        KJ_REQUIRE(false, "capnpc-lean4: unsupported schema node kind",
                   static_cast<int>(proto.which()), proto.getDisplayName());
        break;
    }

    for (auto nested: proto.getNestedNodes()) {
      out += "\n";
      out += genNode(schemaLoader.get(nested.getId()), currentModule);
    }

    return out;
  }

  std::string genFile(Schema fileSchema,
                      kj::StringPtr filename,
                      kj::StringPtr moduleName) {
    std::set<std::string> imports;
    collectImportsForNode(fileSchema, moduleName, imports);

    std::vector<std::string> importList;
    for (const auto& mod: imports) {
      importList.push_back(mod);
    }
    std::sort(importList.begin(), importList.end());

    std::string out;
    out += "-- Generated by capnpc-lean4; DO NOT EDIT.\n";
    out += "-- source: ";
    out += filename.cStr();
    out += "\n\n";

    out += "import Capnp.Runtime\n";
    out += "import Capnp.Rpc\n";
    for (const auto& mod: importList) {
      out += "import ";
      out += mod;
      out += "\n";
    }

    out += "\nset_option maxHeartbeats 2000000\n\n";

    out += "\nnamespace ";
    out += moduleName.cStr();
    out += "\n\n";

    std::vector<Schema> structSchemas;
    std::unordered_set<uint64_t> seenStructs;
    for (auto nested: fileSchema.getProto().getNestedNodes()) {
      collectStructSchemas(schemaLoader.get(nested.getId()), structSchemas, seenStructs);
    }
    auto recursiveStructIds = findRecursiveStructs(structSchemas);
    for (auto schema: structSchemas) {
      out += genStructPrelude(schema, moduleName);
      out += "\n\n";
    }

    bool wroteAlias = false;
    for (auto schema: structSchemas) {
      auto id = schema.getProto().getId();
      if (recursiveStructIds.find(id) == recursiveStructIds.end()) continue;
      out += genStructAlias(schema, moduleName);
      out += "\n";
      wroteAlias = true;
    }
    if (wroteAlias) {
      out += "\n";
    }

    for (auto nested: fileSchema.getProto().getNestedNodes()) {
      auto nonStructOut = genNonStructNodes(schemaLoader.get(nested.getId()), moduleName);
      if (!nonStructOut.empty()) {
        out += nonStructOut;
        out += "\n";
      }
    }

    for (auto schema: structSchemas) {
      auto unionOut = genStructUnion(schema, moduleName);
      if (!unionOut.empty()) {
        out += unionOut;
        out += "\n";
      }
    }

    auto indentAndAppend = [&](const std::string& text) {
      out += "  ";
      for (size_t i = 0; i < text.size(); ++i) {
        out += text[i];
        if (text[i] == '\n' && i + 1 < text.size()) {
          out += "  ";
        }
      }
    };

    std::vector<Schema> valueStructSchemas;
    valueStructSchemas.reserve(structSchemas.size());
    for (auto schema: structSchemas) {
      auto id = schema.getProto().getId();
      if (recursiveStructIds.find(id) != recursiveStructIds.end()) continue;
      valueStructSchemas.push_back(schema);
    }

    if (!valueStructSchemas.empty()) {
      out += "\nmutual\n";
      for (auto schema: valueStructSchemas) {
        auto defText = genStructType(schema, moduleName);
        indentAndAppend(defText);
        out += "\n\n";
      }
      out += "end\n\n";
    }

    for (auto schema: structSchemas) {
      out += genStruct(schema, moduleName);
      out += "\n";
    }

    if (!structSchemas.empty()) {
      out += "\nmutual\n";
      for (auto schema: structSchemas) {
        auto id = schema.getProto().getId();
        auto recursiveStruct = recursiveStructIds.find(id) != recursiveStructIds.end();
        auto defText = genSetFromValue(schema, moduleName, recursiveStruct);
        indentAndAppend(defText);
        out += "\n\n";
      }
      out += "end\n\n";
    }

    for (auto schema: structSchemas) {
      auto deferredSetters = genDeferredValueListSetters(schema, moduleName);
      if (!deferredSetters.empty()) {
        out += deferredSetters;
        out += "\n";
      }
    }

    if (!structSchemas.empty()) {
      out += "\nmutual\n";
      for (auto schema: structSchemas) {
        auto id = schema.getProto().getId();
        auto recursiveStruct = recursiveStructIds.find(id) != recursiveStructIds.end();
        auto defText = genOfReader(schema, moduleName, recursiveStruct);
        indentAndAppend(defText);
        out += "\n\n";
      }
      out += "end\n\n";
    }

    for (auto nested: fileSchema.getProto().getNestedNodes()) {
      auto constOut = genConstNodes(schemaLoader.get(nested.getId()), moduleName);
      if (!constOut.empty()) {
        out += constOut;
        out += "\n";
      }
    }

    out += "end ";
    out += moduleName.cStr();
    out += "\n";
    return out;
  }

  void writeFile(kj::StringPtr filename, const kj::StringTree& text) {
    auto path = kj::Path::parse(filename);
    auto file = fs->getCurrent().openFile(path,
        kj::WriteMode::CREATE | kj::WriteMode::MODIFY | kj::WriteMode::CREATE_PARENT);
    file->writeAll(text.flatten());
  }
};

}  // namespace
}  // namespace capnp

KJ_MAIN(capnp::CapnpcLean4Main);
