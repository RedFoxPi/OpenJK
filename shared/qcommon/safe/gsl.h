#pragma once

// central point of include for the small subset of Microsoft GSL-like
// types this codebase relies on; all of them now have direct standard
// library equivalents, so this header defines them instead of pulling
// in the (unmaintained, pre-C++14) bundled gsl-lite.

#include <cstddef>
#include <span>
#include <string_view>

namespace gsl
{
	/// non-owning view over a contiguous range of const char, not necessarily null-terminated
	using cstring_view = std::string_view;
	/// non-owning pointer to a null-terminated string
	using czstring = const char*;
	/// non-owning view over a contiguous range of T
	template< typename T >
	using array_view = std::span< T >;
}

#define CSTRING_VIEW(x) x ## _v
/** gsl::cstring_view from string literal (without null-termination) */
inline gsl::cstring_view operator"" _v( const char* str, std::size_t length )
{
	return gsl::cstring_view( str, length );
}
