//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: c++03 || c++11 || c++14 || c++17 || c++20
// ADDITIONAL_COMPILE_FLAGS: -freflection -fmetaclasses

// <meta>
//
// Metaclasses: 'class(M) C { ... }'
//
// RUN: %{build}
// RUN: %{exec} %t.exe

#include <meta>

#include <cassert>
#include <type_traits>

using namespace std::meta;
using namespace std::meta::metaclasses;

                                // =========
                                // interface
                                // =========

class(interface) Shape {
  double area();
  void scale(double factor);
};

static_assert(std::is_abstract_v<Shape>);
static_assert(std::has_virtual_destructor_v<Shape>);
static_assert(!std::is_copy_constructible_v<Shape>);
static_assert(is_public(members_of(^^Shape, access_context::unchecked())[0]));

class Square : public Shape {
  double side_;

public:
  explicit Square(double side) : side_(side) {}
  double area() override { return side_ * side_; }
  void scale(double factor) override { side_ *= factor; }
};

static_assert(!std::is_abstract_v<Square>);

                                  // =====
                                  // value
                                  // =====

class(value) Point {
  int x;
  int y;
  int sum() const { return x + y; }
};

static_assert(std::is_standard_layout_v<Point>);
static_assert(is_public(^^Point::x));
static_assert(is_public(^^Point::sum));

                                 // =======
                                 // ordered
                                 // =======

class(ordered) Version {
  int major;
  int minor;
};

static_assert(Version{1, 2} == Version{1, 2});
static_assert(Version{1, 2} < Version{1, 3});
static_assert(Version{2, 0} > Version{1, 9});

                              // ================
                              // user metaclasses
                              // ================

// A metaclass that generates a getter for each data member.
consteval void with_getters(info proto) {
  string code = "public: ";
  for (info m : members_of(proto, access_context::unchecked())) {
    if (!is_nonstatic_data_member(m))
      continue;
    code += "auto get_" + std::string(identifier_of(m)) + "() const { return " +
            std::string(identifier_of(m)) + "; } ";
  }
  inject_members(proto, code);
}

class(with_getters) Person {
  int age;
  char initial;

public:
  Person(int a, char i) : age(a), initial(i) {}
};

struct(plain_struct) Pod {
  int a;
  float b;
};
static_assert(std::is_trivial_v<Pod>);

int main(int, char**) {
  Square sq(3.0);
  Shape &s = sq;
  assert(s.area() == 9.0);
  s.scale(2.0);
  assert(s.area() == 36.0);

  Point p{1, 2};
  assert(p.sum() == 3);

  Person per(42, 'x');
  assert(per.get_age() == 42);
  assert(per.get_initial() == 'x');
  return 0;
}
