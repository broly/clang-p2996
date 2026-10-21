//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: c++03 || c++11 || c++14 || c++17 || c++20
// ADDITIONAL_COMPILE_FLAGS: -freflection -fmetaclasses -Xclang -verify

// <meta>
//
// Diagnostics for metaclasses.

#include <meta>

using namespace std::meta;
using namespace std::meta::metaclasses;

class(interface) HasData { // expected-error {{application of metaclass to 'HasData' failed}}
  int x;
};

class(value) Virtual { // expected-error {{application of metaclass to 'Virtual' failed}}
  virtual void f();
};

template <typename T>
class(interface) Tmpl {}; // expected-error {{metaclass application is not supported on a template}}

class(interface) Fwd; // expected-error {{metaclass application is not supported on a declaration that is not a definition}}

// expected-note@* 0+ {{}}
