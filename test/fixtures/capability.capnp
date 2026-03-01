@0xadc0ffee1badc0de;

interface Dummy {}

struct Small {
  x @0 :UInt32;
}

struct CapHolder {
  cap @0 :Dummy;
  caps @1 :List(Dummy);
}

struct AnyHolder {
  any @0 :AnyPointer;
}
