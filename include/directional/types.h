#pragma once
namespace directional {
// Fundamental fixed-width types keep the same core buildable without a C++
// runtime/STL dependency in kernel mode. Compile-time checks enforce the ABI.
using int32 = signed int;
using uint32 = unsigned int;
using int64 = signed long long;
using uint64 = unsigned long long;
using size_type = decltype(sizeof(0));
static_assert(sizeof(int32)==4 && sizeof(uint32)==4 && sizeof(int64)==8 && sizeof(uint64)==8,
              "Unsupported integer widths");
}
