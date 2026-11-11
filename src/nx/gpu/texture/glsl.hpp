// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// GLSL compute shaders run on the CPU: a thin C++ layer with GLSL's scalar and vector types, its built-in functions
// and a dispatcher, so a test can include a shader's body unchanged and compare what it computes with a reference
// (the same file is compiled into SPIR-V for the GPU by cmake/glsl.cmake). The runtime runs one body this way too:
// the BC7 encoder of the background encodes on the CPU (gpu/texture/bc7_cpu.cpp), whose blocks are the GPU's.
//
// A shader is split in two: the .comp file has the #version line, the layout declarations (workgroup size, push
// constants, buffers) and an #include of the body, and the body is the part of GLSL that is also C++. The test
// includes the body inside a struct that derives from glsl::Invocation and declares, under the same names, what the
// .comp declares: push constant members as plain members, a buffer's array as a pointer. Each invocation is a fresh
// struct, so the body's globals are each invocation's own, as GLSL's private globals are:
//
//     struct Decoder : glsl::Invocation {
//         uint width, height;
//         const glsl::uvec4* astc_data;
//         uint* texels;
//     #include "nx/gpu/texture/astc_decode.glsl"
//     };
//     glsl::dispatch<Decoder>(groups, local_size, [&](Decoder& d) { d.width = ...; });
//
// The body has to keep to what both languages read the same way:
// - constants used as case labels are #defines (a const member is not a constant expression);
// - global arrays are initialized with braces, uint a[2] = {1u, 2u}; never with GLSL's uint[](...);
// - no out or inout parameters, and arrays are never passed, returned or assigned whole (in C++ they decay);
// - vectors are read and written one component at a time (.x or [i]); multi-component swizzles (.xy) are C++
//   compile errors, which is how a body that strays is caught;
// - signed integers do not overflow (it is undefined in C++, and -fwrapv is banned), and literals carry the u suffix
//   where GLSL would make them uint;
// - shared memory is reached through a macro both sides define: the .comp indexes its shared array, the test's
//   struct a pointer to the workgroup's storage, which dispatch_workgroups gives each invocation. barrier() and
//   memoryBarrierShared() are WorkgroupInvocation's: there each invocation runs on a thread of its own, and they
//   meet at a std::barrier, so a body that races on shared memory races in the test too (ThreadSanitizer sees it).
//   As in GLSL, every invocation reaches each barrier();
// - no images or samplers, and floating point only where its results are the same at any precision (below): the
//   layer covers the integer compute shaders the tests need, and those whose floats hold small integers.
//
// A buffer is the device's memory, which is little-endian (the GPU's order) on any host: the test's struct declares
// a buffer's array as leuint (or leuvec4) rather than uint, each word held in the device's byte order and converted
// as the body loads and stores it, so the bytes the test hands a CPU reference, or reads back, are the device's. On a
// little-endian host these are plain words.
#ifndef SNX_GLSL_HPP
#define SNX_GLSL_HPP
#include "common/utilities/bits.h"
#include <barrier>
#include <bit>
#include <cmath>
#include <cstdint>
#include <thread>
#include <type_traits>
#include <vector>

namespace glsl {

using uint = unsigned int;
static_assert(sizeof(uint) == 4 && sizeof(int) == 4, "GLSL's int and uint are 32 bits");

// vectors of 2, 3 or 4 components: x, y, z, w and [i]
template <class T, int N> struct vec;

#define SNX_GLSL_VEC_COMMON(N)                                                                                         \
    using value_type = T;                                                                                              \
    vec() = default;                                                                                                   \
    /* one scalar for every component, as GLSL's uvec4(0) */                                                         \
    template <class S, class = std::enable_if_t<std::is_arithmetic_v<S>>>                                              \
    explicit vec(S s) {                                                                                                \
        for (int i = 0; i < N; i++) (*this)[i] = T(s);                                                                 \
    }                                                                                                                  \
    /* a vector of another component type, converted per component as GLSL's uvec4(ivec4) */                        \
    template <class U, class = std::enable_if_t<!std::is_same_v<U, T>>>                                               \
    explicit vec(const vec<U, N>& v) {                                                                                 \
        for (int i = 0; i < N; i++) (*this)[i] = T(v[i]);                                                              \
    }

template <class T> struct vec<T, 2> {
    T x, y;
    SNX_GLSL_VEC_COMMON(2)
    vec(T x_, T y_) : x(x_), y(y_) {}
    T& operator[](int i) { return i == 0 ? x : y; }
    const T& operator[](int i) const { return i == 0 ? x : y; }
};
template <class T> struct vec<T, 3> {
    T x, y, z;
    SNX_GLSL_VEC_COMMON(3)
    vec(T x_, T y_, T z_) : x(x_), y(y_), z(z_) {}
    T& operator[](int i) { return i == 0 ? x : i == 1 ? y : z; }
    const T& operator[](int i) const { return i == 0 ? x : i == 1 ? y : z; }
};
template <class T> struct vec<T, 4> {
    T x, y, z, w;
    SNX_GLSL_VEC_COMMON(4)
    vec(T x_, T y_, T z_, T w_) : x(x_), y(y_), z(z_), w(w_) {}
    T& operator[](int i) { return i == 0 ? x : i == 1 ? y : i == 2 ? z : w; }
    const T& operator[](int i) const { return i == 0 ? x : i == 1 ? y : i == 2 ? z : w; }
};
#undef SNX_GLSL_VEC_COMMON

using uvec2 = vec<uint, 2>;
using uvec3 = vec<uint, 3>;
using uvec4 = vec<uint, 4>;
using ivec2 = vec<int, 2>;
using ivec3 = vec<int, 3>;
using ivec4 = vec<int, 4>;
using bvec2 = vec<bool, 2>;

// a buffer's 32-bit word: the device's (little-endian) bytes, read and written as a uint
inline uint le_order(uint v) {
    if constexpr (std::endian::native == std::endian::big)
        return snx_bswap32(v);
    else
        return v;
}
struct leuint {
    uint raw; // the word's bytes in the device's order
    leuint() = default;
    leuint(uint v) : raw(le_order(v)) {}
    operator uint() const { return le_order(raw); }
    leuint& operator=(uint v) {
        raw = le_order(v);
        return *this;
    }
    leuint& operator|=(uint v) { return *this = uint(*this) | v; }
    leuint& operator&=(uint v) { return *this = uint(*this) & v; }
    leuint& operator^=(uint v) { return *this = uint(*this) ^ v; }
    leuint& operator+=(uint v) { return *this = uint(*this) + v; }
};
static_assert(sizeof(leuint) == 4, "a buffer word is 4 bytes");
// a buffer's uvec4, four such words
struct leuvec4 {
    leuint x, y, z, w;
    leuvec4() = default;
    leuvec4(const uvec4& v) : x(v.x), y(v.y), z(v.z), w(v.w) {}
    operator uvec4() const { return uvec4(uint(x), uint(y), uint(z), uint(w)); }
};
static_assert(sizeof(leuvec4) == 16, "a buffer uvec4 is 16 bytes");

// component-wise operators, vector with vector and vector with scalar
#define SNX_GLSL_BINARY(op)                                                                                            \
    template <class T, int N> vec<T, N> operator op(vec<T, N> a, const vec<T, N>& b) {                                 \
        for (int i = 0; i < N; i++) a[i] = T(a[i] op b[i]);                                                            \
        return a;                                                                                                      \
    }                                                                                                                  \
    template <class T, int N> vec<T, N> operator op(vec<T, N> a, T b) {                                                \
        for (int i = 0; i < N; i++) a[i] = T(a[i] op b);                                                               \
        return a;                                                                                                      \
    }                                                                                                                  \
    template <class T, int N> vec<T, N> operator op(T a, vec<T, N> b) {                                                \
        for (int i = 0; i < N; i++) b[i] = T(a op b[i]);                                                               \
        return b;                                                                                                      \
    }                                                                                                                  \
    template <class T, int N, class B> vec<T, N>& operator op##=(vec<T, N>& a, const B & b) {                          \
        return a = a op b;                                                                                             \
    }
SNX_GLSL_BINARY(+)
SNX_GLSL_BINARY(-)
SNX_GLSL_BINARY(*)
SNX_GLSL_BINARY(/)
SNX_GLSL_BINARY(%)
SNX_GLSL_BINARY(&)
SNX_GLSL_BINARY(|)
SNX_GLSL_BINARY(^)
SNX_GLSL_BINARY(<<)
SNX_GLSL_BINARY(>>)
#undef SNX_GLSL_BINARY

template <class T, int N> vec<T, N> operator~(vec<T, N> a) {
    for (int i = 0; i < N; i++) a[i] = T(~a[i]);
    return a;
}
template <class T, int N> vec<T, N> operator-(vec<T, N> a) {
    for (int i = 0; i < N; i++) a[i] = T(-a[i]);
    return a;
}
// GLSL's == and != on vectors compare every component
template <class T, int N> bool operator==(const vec<T, N>& a, const vec<T, N>& b) {
    for (int i = 0; i < N; i++)
        if (a[i] != b[i]) return false;
    return true;
}
template <class T, int N> bool operator!=(const vec<T, N>& a, const vec<T, N>& b) { return !(a == b); }

// a built-in of one or more scalars, extended to vectors component by component
#define SNX_GLSL_PER_COMPONENT1(name)                                                                                  \
    template <class T, int N> auto name(const vec<T, N>& a) {                                                          \
        vec<decltype(name(a[0])), N> r;                                                                                \
        for (int i = 0; i < N; i++) r[i] = name(a[i]);                                                                 \
        return r;                                                                                                      \
    }

// The integer built-ins (GLSL 4.60 section 8.8), defined for every input GLSL defines them for
inline uint bitfieldExtract(uint value, int offset, int bits) {
    if (bits <= 0) return 0u;
    return uint((uint64_t(value) >> offset) & ((uint64_t(1) << bits) - 1u));
}
// the signed form sign-extends the field's top bit
inline int bitfieldExtract(int value, int offset, int bits) {
    if (bits <= 0) return 0;
    const uint field = bitfieldExtract(uint(value), offset, bits);
    const uint sign = 1u << (bits - 1);
    return int((field ^ sign) - sign);
}
inline uint bitfieldInsert(uint base, uint insert, int offset, int bits) {
    if (bits <= 0) return base;
    const uint mask = uint(((uint64_t(1) << bits) - 1u) << offset);
    return (base & ~mask) | ((insert << offset) & mask);
}
inline int bitfieldInsert(int base, int insert, int offset, int bits) {
    return int(bitfieldInsert(uint(base), uint(insert), offset, bits));
}
inline uint bitfieldReverse(uint v) {
    v = ((v >> 1) & 0x55555555u) | ((v & 0x55555555u) << 1);
    v = ((v >> 2) & 0x33333333u) | ((v & 0x33333333u) << 2);
    v = ((v >> 4) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4);
    v = ((v >> 8) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8);
    return (v >> 16) | (v << 16);
}
inline int bitfieldReverse(int v) { return int(bitfieldReverse(uint(v))); }
inline int bitCount(uint v) {
    int n = 0;
    for (; v; v &= v - 1u) n++;
    return n;
}
inline int bitCount(int v) { return bitCount(uint(v)); }
// -1 for zero
inline int findLSB(uint v) {
    if (!v) return -1;
    int n = 0;
    while (!(v & 1u)) v >>= 1, n++;
    return n;
}
inline int findLSB(int v) { return findLSB(uint(v)); }
inline int findMSB(uint v) {
    int n = -1;
    for (; v; v >>= 1) n++;
    return n;
}
// the highest bit that differs from the sign bit: -1 for 0 and -1
inline int findMSB(int v) { return findMSB(uint(v < 0 ? ~v : v)); }
SNX_GLSL_PER_COMPONENT1(bitfieldReverse)
SNX_GLSL_PER_COMPONENT1(bitCount)
SNX_GLSL_PER_COMPONENT1(findLSB)
SNX_GLSL_PER_COMPONENT1(findMSB)
#undef SNX_GLSL_PER_COMPONENT1
template <class T, int N> vec<T, N> bitfieldExtract(vec<T, N> v, int offset, int bits) {
    for (int i = 0; i < N; i++) v[i] = bitfieldExtract(v[i], offset, bits);
    return v;
}
template <class T, int N> vec<T, N> bitfieldInsert(vec<T, N> b, const vec<T, N>& s, int offset, int bits) {
    for (int i = 0; i < N; i++) b[i] = bitfieldInsert(b[i], s[i], offset, bits);
    return b;
}
inline void umulExtended(uint x, uint y, uint& msb, uint& lsb) {
    const uint64_t p = uint64_t(x) * y;
    msb = uint(p >> 32);
    lsb = uint(p);
}
inline uint uaddCarry(uint x, uint y, uint& carry) {
    carry = uint(x + y < x);
    return x + y;
}

// min, max and clamp: of two scalars of one type, of vectors, and of a vector with scalars
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>> T min(T a, T b) { return b < a ? b : a; }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>> T max(T a, T b) { return a < b ? b : a; }
template <class T, class = std::enable_if_t<std::is_arithmetic_v<T>>> T clamp(T v, T lo, T hi) {
    return min(max(v, lo), hi);
}
template <class T, int N> vec<T, N> min(vec<T, N> a, const vec<T, N>& b) {
    for (int i = 0; i < N; i++) a[i] = min(a[i], b[i]);
    return a;
}
template <class T, int N> vec<T, N> max(vec<T, N> a, const vec<T, N>& b) {
    for (int i = 0; i < N; i++) a[i] = max(a[i], b[i]);
    return a;
}
template <class T, int N> vec<T, N> min(vec<T, N> a, T b) { return min(a, vec<T, N>(b)); }
template <class T, int N> vec<T, N> max(vec<T, N> a, T b) { return max(a, vec<T, N>(b)); }
template <class T, int N> vec<T, N> clamp(const vec<T, N>& v, T lo, T hi) { return min(max(v, lo), hi); }
template <class T, int N> vec<T, N> clamp(const vec<T, N>& v, const vec<T, N>& lo, const vec<T, N>& hi) {
    return min(max(v, lo), hi);
}

// Floating point. A body's floats are of relaxed precision where a GPU may compute them in 16 bits, so it declares
// them through macros of its own (astc_encode.glsl's AE_F and AE_V) and a test runs it twice, with float and with
// half, a float every operation of which rounds to IEEE binary16 (to nearest, ties to even: what such a GPU
// computes), and compares the bytes: a body whose results hang on the precision fails there. Literals are written as
// GLSL's (0.25), which C++ reads as double: half and the vector operators below take them.
inline double round_binary16(double v) {
    double a = std::fabs(v);
    if (std::isnan(v) || std::isinf(v) || a == 0) return v;
    // the midpoint of the largest value, 65504, and 2^16, which ties to the even one, out of range
    if (a >= 65520.0) return v < 0 ? -HUGE_VAL : HUGE_VAL;
    int e;
    (void)std::frexp(a, &e); // a in [2^(e - 1), 2^e): 11 bits from there, or the subnormals' 2^-24
    e = e - 1 < -14 ? -14 : e - 1;
    double u = std::ldexp(1.0, e - 10), q = a / u, f = std::floor(q), d = q - f;
    if (d > 0.5 || (d == 0.5 && std::fmod(f, 2.0) != 0)) f += 1;
    return std::copysign(f * u, v);
}
struct half {
    double v; // a binary16 value
    half() = default;
    half(double d) : v(round_binary16(d)) {} // not explicit: a body's literals and float(x) become half
    explicit operator uint() const { return uint(v); }
    explicit operator int() const { return int(v); }
    explicit operator double() const { return v; }
    explicit operator float() const { return float(v); }
};
// the sum, difference or product of two binary16 values is exact in a double but for its last place far below
// binary16's, so rounding it is rounding the exact result
inline half operator+(half a, half b) { return half(a.v + b.v); }
inline half operator-(half a, half b) { return half(a.v - b.v); }
inline half operator*(half a, half b) { return half(a.v * b.v); }
inline half operator-(half a) { return half(-a.v); }
inline bool operator==(half a, half b) { return a.v == b.v; }
inline bool operator!=(half a, half b) { return a.v != b.v; }
inline bool operator<(half a, half b) { return a.v < b.v; }
inline bool operator>(half a, half b) { return a.v > b.v; }
inline bool operator<=(half a, half b) { return a.v <= b.v; }
inline bool operator>=(half a, half b) { return a.v >= b.v; }
inline half min(half a, half b) { return b < a ? b : a; }
inline half max(half a, half b) { return a < b ? b : a; }
inline half clamp(half v, half lo, half hi) { return min(max(v, lo), hi); }
inline float floor(float a) { return std::floor(a); }
inline half floor(half a) { return half(std::floor(a.v)); }
inline float abs(float a) { return std::fabs(a); }
inline half abs(half a) { return half(std::fabs(a.v)); }
inline float sign(float a) { return float((a > 0) - (a < 0)); }
inline half sign(half a) { return half(double((a.v > 0) - (a.v < 0))); }
inline float step(float edge, float x) { return x < edge ? 0.0f : 1.0f; }
inline half step(half edge, half x) { return half(x < edge ? 0.0 : 1.0); }
template <class T> inline constexpr bool is_real = std::is_same_v<T, float> || std::is_same_v<T, half>;
#define SNX_GLSL_REAL1(name)                                                                                           \
    template <class T, int N, class = std::enable_if_t<is_real<T>>> vec<T, N> name(vec<T, N> a) {                      \
        for (int i = 0; i < N; i++) a[i] = name(a[i]);                                                                 \
        return a;                                                                                                      \
    }
SNX_GLSL_REAL1(floor)
SNX_GLSL_REAL1(abs)
SNX_GLSL_REAL1(sign)
#undef SNX_GLSL_REAL1
template <class T, int N, class = std::enable_if_t<is_real<T>>> vec<T, N> step(const vec<T, N>& edge, vec<T, N> x) {
    for (int i = 0; i < N; i++) x[i] = step(edge[i], x[i]);
    return x;
}
template <class T, int N, class = std::enable_if_t<is_real<T>>> T dot(const vec<T, N>& a, const vec<T, N>& b) {
    T sum = a[0] * b[0];
    for (int i = 1; i < N; i++) sum = sum + a[i] * b[i];
    return sum;
}
// a vector times a literal
template <class T, int N, class = std::enable_if_t<is_real<T>>> vec<T, N> operator*(vec<T, N> a, double b) {
    for (int i = 0; i < N; i++) a[i] = a[i] * T(b);
    return a;
}

// the built-in inputs of a compute invocation
struct Invocation {
    uvec3 gl_NumWorkGroups, gl_WorkGroupSize, gl_WorkGroupID, gl_LocalInvocationID, gl_GlobalInvocationID;
    uint gl_LocalInvocationIndex;
};

// the atomic read-modify-writes on a buffer's word (GLSL 4.60 section 8.11), which return the old value. The
// builtins, not std::atomic_ref: the Android NDK's libc++ (before LLVM 19) has none
inline uint atomicAnd(uint& mem, uint data) { return __atomic_fetch_and(&mem, data, __ATOMIC_SEQ_CST); }
inline uint atomicOr(uint& mem, uint data) { return __atomic_fetch_or(&mem, data, __ATOMIC_SEQ_CST); }
// on a buffer word in the device's order: a bitwise operation is the same on the bytes in either order
inline uint atomicAnd(leuint& mem, uint data) { return le_order(atomicAnd(mem.raw, le_order(data))); }
inline uint atomicOr(leuint& mem, uint data) { return le_order(atomicOr(mem.raw, le_order(data))); }

// runs main() of every invocation of groups workgroups of local_size each, in order. setup(shader) fills in each
// invocation's push constants and buffers before it runs
template <class Shader, class Setup> void dispatch(uvec3 groups, uvec3 local_size, Setup&& setup) {
    for (uint gz = 0; gz < groups.z; gz++)
        for (uint gy = 0; gy < groups.y; gy++)
            for (uint gx = 0; gx < groups.x; gx++)
                for (uint lz = 0; lz < local_size.z; lz++)
                    for (uint ly = 0; ly < local_size.y; ly++)
                        for (uint lx = 0; lx < local_size.x; lx++) {
                            Shader s;
                            setup(s);
                            s.gl_NumWorkGroups = groups;
                            s.gl_WorkGroupSize = local_size;
                            s.gl_WorkGroupID = uvec3(gx, gy, gz);
                            s.gl_LocalInvocationID = uvec3(lx, ly, lz);
                            s.gl_GlobalInvocationID = uvec3(gx * local_size.x + lx, gy * local_size.y + ly,
                                                            gz * local_size.z + lz);
                            s.gl_LocalInvocationIndex = (lz * local_size.y + ly) * local_size.x + lx;
                            s.main();
                        }
}

// an invocation of a shader with barrier(): its workgroup's invocations run on threads of their own
struct WorkgroupInvocation : Invocation {
    std::barrier<>* workgroup_barrier = nullptr;
    void barrier() { workgroup_barrier->arrive_and_wait(); }
    void memoryBarrierShared() {} // the std::barrier orders memory already
};

// runs main() of every invocation of groups workgroups of local_size each, a workgroup at a time, its invocations
// on threads of their own that meet at barrier(). Each workgroup gets a value-initialized Shared, and setup(shader,
// shared) fills in each invocation's push constants and buffers, and points it at the workgroup's shared memory
template <class Shader, class Shared, class Setup> void dispatch_workgroups(uvec3 groups, uvec3 local_size, Setup&& setup) {
    const uint n = local_size.x * local_size.y * local_size.z;
    for (uint gz = 0; gz < groups.z; gz++)
        for (uint gy = 0; gy < groups.y; gy++)
            for (uint gx = 0; gx < groups.x; gx++) {
                Shared shared{};
                std::barrier<> meet(n);
                std::vector<std::thread> threads;
                threads.reserve(n);
                for (uint index = 0; index < n; index++)
                    threads.emplace_back([&, index, gx, gy, gz] {
                        Shader s;
                        setup(s, shared);
                        const uint lx = index % local_size.x, ly = index / local_size.x % local_size.y,
                                   lz = index / (local_size.x * local_size.y);
                        s.gl_NumWorkGroups = groups;
                        s.gl_WorkGroupSize = local_size;
                        s.gl_WorkGroupID = uvec3(gx, gy, gz);
                        s.gl_LocalInvocationID = uvec3(lx, ly, lz);
                        s.gl_GlobalInvocationID = uvec3(gx * local_size.x + lx, gy * local_size.y + ly,
                                                        gz * local_size.z + lz);
                        s.gl_LocalInvocationIndex = index;
                        s.workgroup_barrier = &meet;
                        s.main();
                    });
                for (std::thread& t : threads) t.join();
            }
}

} // namespace glsl
#endif
