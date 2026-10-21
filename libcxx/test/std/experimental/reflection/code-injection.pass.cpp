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
// Code injection with token sequences: '^^{ ... }' and 'queue_injection'.
//
// RUN: %{build}
// RUN: %{exec} %t.exe

#include <meta>

#include <cassert>
#include <string>

using namespace std::meta;

namespace ns {
struct Tag {};
}

                           // ==============
                           // plain sequence
                           // ==============

struct Plain {
  consteval {
    queue_injection(^^{
      constexpr int answer() const { return 42; }
      static constexpr int two = 2;
    });
  }
};

static_assert(Plain{}.answer() == 42);
static_assert(Plain::two == 2);

                         // ====================
                         // value interpolation
                         // ====================

consteval info make_member(int n) {
  return ^^{ static constexpr int value = \(n) * 2; };
}

struct Valued {
  consteval { queue_injection(make_member(21)); }
};
static_assert(Valued::value == 42);

                         // =================
                         // \id interpolation
                         // =================

struct Ids {
  consteval {
    queue_injection(^^{ constexpr int \id("get_", 7)() const { return 7; } });
    queue_injection(^^{ using \id(identifier_of(^^ns::Tag)) = int; });
  }
};
static_assert(Ids{}.get_7() == 7);
static_assert(std::is_same_v<Ids::Tag, int>);

                       // ======================
                       // type reflection splice
                       // ======================

struct Typed {
  consteval {
    queue_injection(^^{ using type = \(^^ns::Tag); });
  }
};
static_assert(std::is_same_v<Typed::type, ns::Tag>);

struct Explicit;
consteval void inject_into(info target) {
  inject_members(target, ^^{ static constexpr int tag = \(7); using self = \(target); });
}
struct Explicit {
  consteval { inject_into(^^Explicit); }
};
static_assert(Explicit::tag == 7);
static_assert(std::is_same_v<Explicit::self, Explicit>);

                       // ===================
                       // \tokens composition
                       // ===================

consteval info body() { return ^^{ return 5; }; }

struct Composed {
  consteval {
    queue_injection(^^{ constexpr int five() const { \tokens(body()) } });
  }
};
static_assert(Composed{}.five() == 5);

                       // ===================
                       // string interpolation
                       // ===================

struct Strings {
  consteval {
    queue_injection(^^{ static constexpr const char *name = \("hi \"there\""); });
  }
};

                   // ================================
                   // generating members in a metaclass
                   // ================================

consteval void with_setters(info proto) {
  for (info m : members_of(proto, access_context::unchecked())) {
    if (!is_nonstatic_data_member(m))
      continue;
    queue_injection(^^{
      public:
      void \id("set_", identifier_of(m))(\(type_of(m)) v) { \id(identifier_of(m)) = v; }
    });
  }
}

class(with_setters) Settings {
  int width = 0;
  double ratio = 0;

public:
  int w() const { return width; }
  double r() const { return ratio; }
};

int main(int, char**) {
  assert(std::string(Strings::name) == "hi \"there\"");

  Settings s;
  s.set_width(10);
  s.set_ratio(1.5);
  assert(s.w() == 10 && s.r() == 1.5);
  return 0;
}
