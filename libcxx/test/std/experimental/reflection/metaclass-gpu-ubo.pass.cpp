//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// UNSUPPORTED: c++03 || c++11 || c++14 || c++17 || c++20
// ADDITIONAL_COMPILE_FLAGS: -freflection-latest -fmetaclasses

// <meta>
//
// A metaclass for types shared with GPU shaders (UBOs): checks the C++ layout
// of the type, and optionally that it matches std140 / std430.
//
// RUN: %{build}
// RUN: %{exec} %t.exe

#include <meta>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

                              // ==============
                              // test support
                              // ==============

namespace glm {  // same shape as the real glm
template <int L, class T, int Q = 0> struct vec { T v[L]; };
template <int C, int R, class T, int Q = 0> struct mat { vec<R, T, Q> c[C]; };
using vec2 = vec<2, float>;
using vec3 = vec<3, float>;
using vec4 = vec<4, float>;
using uvec4 = vec<4, unsigned>;
using mat4 = mat<4, 4, float>;
}  // namespace glm

namespace reflect {
inline int registered = 0;
template <class T> struct TypeRegistrar { TypeRegistrar() { ++registered; } };
}  // namespace reflect

namespace rh {
struct padding_t {};
inline constexpr padding_t padding{};
struct Std140 {};
struct Std430 {};
}  // namespace rh

                  // ================================
                  // the metaclass (copy this part)
                  // ================================

namespace rh_meta {

using std::meta::info;

enum class Layout { std140 = 140, std430 = 430 };

consteval bool has_annotation(info r, info type) {
  return !std::meta::annotations_of(r, type).empty();
}
template <class T> consteval bool has_annotation(info r) {
  return has_annotation(r, ^^T);
}

consteval bool is_padding(info member) {
  return has_annotation(member, ^^rh::padding_t);
}

consteval size_t align_up(size_t v, size_t a) { return (v + a - 1) / a * a; }

// (std::to_string is not constexpr)
consteval std::string to_str(size_t v) {
  std::string r;
  do {
    r.insert(r.begin(), static_cast<char>('0' + v % 10));
    v /= 10;
  } while (v != 0);
  return r;
}

// Persistent text for a message (the result of a consteval function can't
// refer to transient memory).
consteval std::string_view persist(const std::string &s) {
  return std::string_view(std::define_static_string(s), s.size());
}

                              // ==============
                              // no implicit gaps
                              // ==============

// Returns a description of the first implicit gap (padding that is not a member
// annotated with [[=rh::padding]]) in the complete type 'type', or "".
consteval std::string_view gap_error(info type) {
  using namespace std::meta;
  size_t end = 0;
  for (info m : nonstatic_data_members_of(type, access_context::unchecked())) {
    if (is_bit_field(m))
      return persist("bit-fields are not allowed: " +
                     std::string(identifier_of(m)));
    size_t at = offset_of(m).bytes;
    if (at != end)
      return persist(std::string("implicit ") + to_str(at - end) +
                     "-byte gap before member '" +
                     std::string(identifier_of(m)) +
                     "'; add an explicit [[=rh::padding]] member");
    end = at + size_of(type_of(m));
  }
  if (end != size_of(type))
    return persist(std::string("implicit ") + to_str(size_of(type) - end) +
                   "-byte tail padding; add an explicit [[=rh::padding]] member");
  return "";
}

                              // ==============
                              // GPU layouts
                              // ==============

struct GpuInfo {
  size_t align = 0;
  size_t size = 0;
  std::string error;  // empty if ok
};

consteval GpuInfo gpu_error(std::string s) { return {0, 0, std::move(s)}; }
consteval GpuInfo gpu_info(info type, Layout layout);

// GLSL layout of a struct: the members that are not padding, in order.
consteval GpuInfo gpu_struct_info(info type, Layout layout, bool check_offsets) {
  using namespace std::meta;
  size_t off = 0;
  size_t max_align = 1;
  for (info m : nonstatic_data_members_of(type, access_context::unchecked())) {
    if (is_padding(m))
      continue;
    GpuInfo mi = gpu_info(type_of(m), layout);
    if (!mi.error.empty())
      return gpu_error("member '" + std::string(identifier_of(m)) + "': " +
                       mi.error);
    off = align_up(off, mi.align);
    if (check_offsets && offset_of(m).bytes != off)
      return gpu_error("member '" + std::string(identifier_of(m)) +
                       "' is at offset " + to_str(offset_of(m).bytes) +
                       " in C++ but GLSL expects offset " +
                       to_str(off));
    off += mi.size;
    if (mi.align > max_align)
      max_align = mi.align;
  }
  // std140 rounds the alignment of a structure up to that of a vec4.
  if (layout == Layout::std140)
    max_align = align_up(max_align, 16);
  return {max_align, align_up(off, max_align), {}};
}

consteval GpuInfo gpu_info(info type, Layout layout) {
  using namespace std::meta;
  type = remove_cv(dealias(type));

  if (is_arithmetic_type(type)) {
    size_t s = size_of(type);
    if (type == ^^bool || (s != 4 && s != 8))
      return gpu_error("type '" + std::string(display_string_of(type)) +
                       "' has no GLSL equivalent");
    return {s, s, {}};
  }

  // Arrays: the stride is the size of the element (rounded up to a vec4 in
  // std140).
  if (is_array_type(type)) {
    GpuInfo e = gpu_info(remove_extent(type), layout);
    if (!e.error.empty())
      return e;
    size_t align = e.align;
    size_t stride = align_up(e.size, e.align);
    if (layout == Layout::std140) {
      align = align_up(align, 16);
      stride = align_up(e.size, 16);
    }
    return {align, stride * extent(type, 0), {}};
  }

  if (has_template_arguments(type) &&
      has_identifier(template_of(type)) &&
      identifier_of(parent_of(template_of(type))) == "glm") {
    std::string_view name = identifier_of(template_of(type));
    auto args = template_arguments_of(type);

    // glm::vec<L, T, Q>
    if (name == "vec") {
      size_t n = extract<int>(args[0]);
      GpuInfo s = gpu_info(args[1], layout);
      if (!s.error.empty())
        return s;
      return {(n == 2 ? 2 : 4) * s.size, n * s.size, {}};
    }

    // glm::mat<C, R, T, Q>: an array of C column vectors of R components.
    if (name == "mat") {
      size_t columns = extract<int>(args[0]);
      size_t rows = extract<int>(args[1]);
      GpuInfo s = gpu_info(args[2], layout);
      if (!s.error.empty())
        return s;
      size_t col_align = (rows == 2 ? 2 : 4) * s.size;
      size_t col_size = rows * s.size;
      size_t align = col_align;
      size_t stride = align_up(col_size, col_align);
      if (layout == Layout::std140) {
        align = align_up(align, 16);
        stride = align_up(col_size, 16);
      }
      return {align, stride * columns, {}};
    }
  }

  if (is_class_type(type)) {
    GpuInfo s = gpu_struct_info(type, layout, /*check_offsets=*/true);
    if (!s.error.empty())
      return s;
    if (size_of(type) != s.size)
      return gpu_error("struct '" + std::string(display_string_of(type)) +
                       "' has size " + to_str(size_of(type)) +
                       " in C++ but GLSL expects " + to_str(s.size));
    return s;
  }

  return gpu_error("type '" + std::string(display_string_of(type)) +
                   "' has no GLSL equivalent");
}

// Returns a description of why the complete type 'type' doesn't have the
// layout 'layout' (members annotated with [[=rh::padding]] are not part of the
// GLSL declaration), or "".
consteval std::string_view gpu_layout_error(info type, Layout layout) {
  using namespace std::meta;
  GpuInfo s = gpu_struct_info(type, layout, /*check_offsets=*/true);
  if (s.error.empty() && size_of(type) != s.size)
    s.error = "size is " + to_str(size_of(type)) +
              " in C++ but GLSL expects " + to_str(s.size);
  if (s.error.empty())
    return "";
  return persist(std::string(layout == Layout::std140 ? "std140" : "std430") +
                 " layout mismatch: " + s.error);
}

}  // namespace rh_meta

consteval void rh_type(std::meta::info target) {
  using namespace std::meta;

  // Checks that can be made while the class is still being defined.
  for (info m : members_of(target, access_context::unchecked()))
    if (is_function(m) && is_virtual(m))
      compiler_error("rh_type: virtual functions are not allowed");

  bool std140 = rh_meta::has_annotation<rh::Std140>(target);
  bool std430 = rh_meta::has_annotation<rh::Std430>(target);
  compiler_require(!(std140 && std430),
                   "rh_type: a type cannot be both Std140 and Std430");

  // Checks that need the complete type are made in the body of a member
  // function, which is parsed once the class is complete.
  queue_injection(^^{
    static inline reflect::TypeRegistrar<\(target)> rh_registrar_;

    static constexpr bool rh_validate_() {
      static_assert(::std::is_standard_layout_v<\(target)>,
                    "rh_type: type must be standard-layout");
      static_assert(::std::is_trivially_copyable_v<\(target)>,
                    "rh_type: type must be trivially copyable");
      static_assert(::rh_meta::gap_error(^^\(target)).empty(),
                    ::rh_meta::gap_error(^^\(target)));
      return true;
    }
  });

  if (std140)
    queue_injection(^^{
      static constexpr bool rh_validate_std140_() {
        static_assert(
            ::rh_meta::gpu_layout_error(^^\(target), ::rh_meta::Layout::std140).empty(),
            ::rh_meta::gpu_layout_error(^^\(target), ::rh_meta::Layout::std140));
        return true;
      }
    });
  if (std430)
    queue_injection(^^{
      static constexpr bool rh_validate_std430_() {
        static_assert(
            ::rh_meta::gpu_layout_error(^^\(target), ::rh_meta::Layout::std430).empty(),
            ::rh_meta::gpu_layout_error(^^\(target), ::rh_meta::Layout::std430));
        return true;
      }
    });
}

                              // ==============
                              // usage
                              // ==============

struct (rh_type) [[=rh::Std140{}]] MaterialUBO {
  float base_color_factor;
  float emissive_factor;
  float occlusion_factor;
  float roughness_factor;
  float metallic_factor;
  [[=rh::padding]] float _pad0[3];
};

struct (rh_type) [[=rh::Std140{}]] LightUBO {
  int light_count;
  [[=rh::padding]] glm::vec3 _pad0;
  glm::vec4 grid_origin;
  glm::mat4 view_proj[2];
  glm::vec3 direction;
  float intensity;
};

struct (rh_type) [[=rh::Std430{}]] Particles {
  glm::vec4 position;
  float life;
  [[=rh::padding]] float _pad[3];
};

struct (rh_type) NoGpu {  // only the generic checks
  double d;
  int i;
  int j;
};

static_assert(sizeof(MaterialUBO) == 32);
static_assert(sizeof(LightUBO) == 16 + 16 + 128 + 16);

int main(int, char**) {
  assert(reflect::registered == 0);  // registrars are variables, not yet used
  return 0;
}
