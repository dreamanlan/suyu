// pragma_diag.h
#pragma once

// PRAGMA_DIAG_* - cross-compiler diagnostic helpers
// Usage:
//   PRAGMA_DIAG_PUSH();
//   PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR(); // or other specific ignore macros
//   ... code ...
//   PRAGMA_DIAG_POP();

// push / pop / per-compiler ignore primitives
#if defined(_MSC_VER)

  #define PRAGMA_DIAG_PUSH()            __pragma(warning(push))
  #define PRAGMA_DIAG_POP()             __pragma(warning(pop))
  #define PRAGMA_DIAG_IGNORE_MSVC(code) __pragma(warning(disable: code))

#elif defined(__clang__) || defined(__GNUC__)

  #define PRAGMA_DIAG_STR_HELPER(x) #x
  #define PRAGMA_DIAG_STR(x)        PRAGMA_DIAG_STR_HELPER(x)
  #define PRAGMA_DIAG_PRAGMA(x)     _Pragma(PRAGMA_DIAG_STR(x))

  #if defined(__clang__)
    #define PRAGMA_DIAG_PUSH()                  PRAGMA_DIAG_PRAGMA(clang diagnostic push)
    #define PRAGMA_DIAG_POP()                   PRAGMA_DIAG_PRAGMA(clang diagnostic pop)
    // Note: the argument here must be a string literal (e.g. "-W..."), see usage below
    #define PRAGMA_DIAG_IGNORE_CLANG(warning_str_literal)   PRAGMA_DIAG_PRAGMA(clang diagnostic ignored warning_str_literal)
  #else /* GCC */
    #define PRAGMA_DIAG_PUSH()                  PRAGMA_DIAG_PRAGMA(GCC diagnostic push)
    #define PRAGMA_DIAG_POP()                   PRAGMA_DIAG_PRAGMA(GCC diagnostic pop)
    // Note: the argument here must be a string literal (e.g. "-W..."), see usage below
    #define PRAGMA_DIAG_IGNORE_GCC(warning_str_literal)     PRAGMA_DIAG_PRAGMA(GCC diagnostic ignored warning_str_literal)
  #endif

#else

  /* Unknown compiler: no-ops so header is portable */
  #define PRAGMA_DIAG_PUSH()
  #define PRAGMA_DIAG_POP()
  #define PRAGMA_DIAG_IGNORE_MSVC(code)
  #define PRAGMA_DIAG_IGNORE_CLANG(warning_str_literal)
  #define PRAGMA_DIAG_IGNORE_GCC(warning_str_literal)

#endif

// ---------------------------
// Convenience cross-compiler aliases
// Use quoted warning names for GCC/Clang (e.g. "-W...")
// ---------------------------

// 1) return-local-addr
//   - GCC:   "-Wreturn-local-addr"
//   - Clang: "-Wreturn-stack-address"
//   - MSVC:  4172
#if defined(_MSC_VER)
  #define PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_MSVC() PRAGMA_DIAG_IGNORE_MSVC(4172)
#else
  #define PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_MSVC()
#endif

#if defined(__clang__)
  #define PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_CLANG() PRAGMA_DIAG_IGNORE_CLANG("-Wreturn-stack-address")
#else
  #define PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_CLANG()
#endif

#if defined(__GNUC__) && !defined(__clang__)
  #define PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_GCC() PRAGMA_DIAG_IGNORE_GCC("-Wreturn-local-addr")
#else
  #define PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_GCC()
#endif

#define PRAGMA_DIAG_PUSH_IGNORE_RETURN_LOCAL_ADDR() \
  PRAGMA_DIAG_PUSH();                                \
  PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_MSVC();       \
  PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_CLANG();      \
  PRAGMA_DIAG_IGNORE_RETURN_LOCAL_ADDR_GCC()

#define PRAGMA_DIAG_POP_IGNORE_RETURN_LOCAL_ADDR() \
  PRAGMA_DIAG_POP()

// 2) unused variable
#if defined(_MSC_VER)
  #define PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_MSVC() PRAGMA_DIAG_IGNORE_MSVC(4101)
#else
  #define PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_MSVC()
#endif

#if defined(__clang__)
  #define PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_CLANG() PRAGMA_DIAG_IGNORE_CLANG("-Wunused-variable")
#else
  #define PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_CLANG()
#endif

#if defined(__GNUC__) && !defined(__clang__)
  #define PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_GCC() PRAGMA_DIAG_IGNORE_GCC("-Wunused-variable")
#else
  #define PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_GCC()
#endif

#define PRAGMA_DIAG_PUSH_IGNORE_UNUSED_VARIABLE() \
  PRAGMA_DIAG_PUSH();                             \
  PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_MSVC();      \
  PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_CLANG();     \
  PRAGMA_DIAG_IGNORE_UNUSED_VARIABLE_GCC()

#define PRAGMA_DIAG_POP_IGNORE_UNUSED_VARIABLE() \
  PRAGMA_DIAG_POP()

// 3) deprecated-declarations
#if defined(_MSC_VER)
  #define PRAGMA_DIAG_IGNORE_DEPRECATED_MSVC() PRAGMA_DIAG_IGNORE_MSVC(4996)
#else
  #define PRAGMA_DIAG_IGNORE_DEPRECATED_MSVC()
#endif

#if defined(__clang__)
  #define PRAGMA_DIAG_IGNORE_DEPRECATED_CLANG() PRAGMA_DIAG_IGNORE_CLANG("-Wdeprecated-declarations")
#else
  #define PRAGMA_DIAG_IGNORE_DEPRECATED_CLANG()
#endif

#if defined(__GNUC__) && !defined(__clang__)
  #define PRAGMA_DIAG_IGNORE_DEPRECATED_GCC() PRAGMA_DIAG_IGNORE_GCC("-Wdeprecated-declarations")
#else
  #define PRAGMA_DIAG_IGNORE_DEPRECATED_GCC()
#endif

#define PRAGMA_DIAG_PUSH_IGNORE_DEPRECATED() \
  PRAGMA_DIAG_PUSH();                        \
  PRAGMA_DIAG_IGNORE_DEPRECATED_MSVC();      \
  PRAGMA_DIAG_IGNORE_DEPRECATED_CLANG();     \
  PRAGMA_DIAG_IGNORE_DEPRECATED_GCC()

#define PRAGMA_DIAG_POP_IGNORE_DEPRECATED() \
  PRAGMA_DIAG_POP()

// ---------------------------
// Notes:
// - For GCC/Clang, the macros that call PRAGMA_DIAG_IGNORE_* must pass a string literal, e.g. "-Wfoo".
// - The convenience macros above already use quoted strings, so Android/NDK clang won't complain.
// - Keep the push/ignore/pop scope as small as possible.
// ---------------------------