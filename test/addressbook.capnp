@0x9d3b7e9c4b1ab2f3;

struct Person {
  id @0 :UInt64;
  name @1 :Text;
  email @2 :Text;
  phones @3 :List(PhoneNumber);

  union {
    employment @4 :Text;
    unemployed @5 :Void;
  }

  struct PhoneNumber {
    number @0 :Text;
    type @1 :PhoneType;
  }

  enum PhoneType {
    mobile @0;
    home @1;
    work @2;
  }
}

struct AddressBook {
  people @0 :List(Person);
}
