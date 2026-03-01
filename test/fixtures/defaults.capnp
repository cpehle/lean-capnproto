@0xebfde6b6263d86b5;

struct Nested {
  num @0 :UInt32;
}

enum Color {
  red @0;
  green @1;
  blue @2;
}

struct Defaults {
  boolField @0 :Bool = true;
  int16Field @1 :Int16 = -123;
  uint32Field @2 :UInt32 = 123456;
  float32Field @3 :Float32 = 1.5;
  float64Field @4 :Float64 = -2.25;
  enumField @5 :Color = green;
  textField @6 :Text = "hello";
  dataField @7 :Data = "abc";
  listUInt16 @8 :List(UInt16) = [10, 20, 30];
  listText @9 :List(Text) = ["a", "b"];
  listListUInt8 @10 :List(List(UInt8)) = [[1, 2], [3]];
  nested @11 :Nested = (num = 7);
  nestedList @12 :List(Nested) = [(num = 1), (num = 2)];
}

const nestedConst :Nested = (num = 99);
const listConst :List(UInt16) = [4, 5];
const listNestedConst :List(Nested) = [(num = 8)];
