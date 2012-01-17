/*=============================================================================
Copyright (c) 2009 Broadcom Europe Limited.
All rights reserved.

Project  :  vcfw
Module   :  osal

FILE DESCRIPTION
VideoCore OS Abstraction Layer - Assertion and error-handling macros.
=============================================================================*/


#ifndef VCOS_ASSERT_H
#define VCOS_ASSERT_H

/*
 * Macro:
 *    vcos_assert(cond)
 *    vcos_assert_msg(cond, fmt, ...)
 * Use:
 *    Detecting programming errors by ensuring that assumptions are correct.
 * On failure:
 *    Performs a platform-dependent "breakpoint", usually with an assert-style
 *    message. The '_msg' variant expects a printf-style format string and
 *    parameters.
 *    If a failure is detected, the code should be fixed and rebuilt.
 * In release builds:
 *    Generates no code, i.e. does not evaluate 'cond'.
 * Returns:
 *    Nothing.
 *
 * Macro:
 *    vcos_demand(cond)
 *    vcos_demand_msg(cond, fmt, ...)
 * Use:
 *    Detecting fatal system errors that require a reboot.
 * On failure:
 *    Performs a platform-dependent "breakpoint", usually with an assert-style
 *    message, then calls vcos_abort (see below).
 * In release builds:
 *    Calls vcos_abort() if 'cond' is false.
 * Returns:
 *    Nothing (never, on failure).
 *
 * Macro:
 *    vcos_verify(cond)
 *    vcos_verify_msg(cond, fmt, ...)
 * Use:
 *    Detecting run-time errors and interesting conditions, normally within an
 *    'if' statement to catch the failures, i.e.
 *       if (!vcos_verify(cond)) handle_error();
 * On failure:
 *    Generates a message and optionally stops at a platform-dependent
 *    "breakpoint" (usually disabled). See vcos_verify_bkpts_enable below.
 * In release builds:
 *    Just evaluates and returns 'cond'.
 * Returns:
 *    Non-zero if 'cond' is true, otherwise zero.
 *
 * Macro:
 *    vcos_static_assert(cond)
 * Use:
 *    Detecting compile-time errors.
 * On failure:
 *    Generates a compiler error.
 * In release builds:
 *    Generates a compiler error.
 *
 * Function:
 *    void vcos_abort(void)
 * Use:
 *    Invokes the fatal error handling mechanism, alerting the host where
 *    applicable.
 * Returns:
 *    Never.
 *
 * Macro:
 *    VCOS_VERIFY_BKPTS
 * Use:
 *    Define in a module (before including vcos.h) to specify an alternative
 *    flag to control breakpoints on vcos_verify() failures.
 * Returns:
 *    Non-zero values enable breakpoints.
 *
 * Function:
 *    int vcos_verify_bkpts_enable(int enable);
 * Use:
 *    Sets the global flag controlling breakpoints on vcos_verify failures,
 *    enabling the breakpoints iff 'enable' is non-zero.
 * Returns:
 *    The previous state of the flag.
 *
 * Function:
 *    int vcos_verify_bkpts_enabled(void);
 * Use:
 *    Queries the state of the global flag enabling breakpoints on vcos_verify
 *    failures.
 * Returns:
 *    The current state of the flag.
 *
 * Examples:
 *
 * int my_breakpoint_enable_flag = 1;
 *
 * #define VCOS_VERIFY_BKPTS my_breakpoint_enable_flag
 *
 * #include "interface/vcos/vcos.h"
 *
 * vcos_static_assert((sizeof(object) % 32) == 0);
 *
 * // ...
 *
 *    vcos_assert_msg(postcondition_is_true, "Coding error");
 *
 *    if (!vcos_verify_msg(buf, "Buffer allocation failed (%d bytes)", size))
 *    {
 *       // Tidy up
 *       // ...
 *       return OUT_OF_MEMORY;
 *    }
 *
 *    vcos_demand(*p++==GUARDWORDHEAP);
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "interface/vcos/vcos_types.h"

#ifdef __COVERITY__
#undef VCOS_ASSERT_BKPT
#define VCOS_ASSERT_BKPT __coverity_panic__()
#endif

#ifndef VCOS_VERIFY_BKPTS
#define VCOS_VERIFY_BKPTS vcos_verify_bkpts_enabled()
#endif

#ifndef VCOS_BKPT
#if defined(__VIDEOCORE__) && !defined(VCOS_ASSERT_NO_BKPTS)
#define VCOS_BKPT _bkpt()
#else
#define VCOS_BKPT (void )0
#endif
#endif

#ifndef VCOS_ASSERT_BKPT
#define VCOS_ASSERT_BKPT VCOS_BKPT
#endif

#ifndef VCOS_VERIFY_BKPT
#define VCOS_VERIFY_BKPT (VCOS_VERIFY_BKPTS ? VCOS_BKPT : (void)0)
#endif

VCOSPRE_ int VCOSPOST_ vcos_verify_bkpts_enabled(void);
VCOSPRE_ int VCOSPOST_ vcos_verify_bkpts_enable(int enable);
VCOSPRE_ void VCOSPOST_ vcos_abort(void);

#ifndef VCOS_ASSERT_MSG
#ifdef LOGGING
extern void logging_assert(const char *file, const char *func, int line, const char *format, ...);
#define VCOS_ASSERT_MSG(...) ((VCOS_ASSERT_LOGGING && !VCOS_ASSERT_LOGGING_DISABLE) ? logging_assert(__FILE__, __func__, __LINE__, __VA_ARGS__) : (void)0)
#else
#define VCOS_ASSERT_MSG(...) ((void)0)
#endif
#endif

#ifndef VCOS_VERIFY_MSG
#define VCOS_VERIFY_MSG(...) VCOS_ASSERT_MSG(__VA_ARGS__)
#endif

#ifndef VCOS_ASSERT_LOGGING
#define VCOS_ASSERT_LOGGING 0
#endif

#ifndef VCOS_ASSERT_LOGGING_DISABLE
#define VCOS_ASSERT_LOGGING_DISABLE 0
#endif

#if !defined(NDEBUG) || defined(VCOS_RELEASE_ASSERTS)

#ifndef vcos_assert
#define vcos_assert(cond) \
   ( (cond) ? (void)0 : (VCOS_ASSERT_MSG("%s", #cond), VCOS_ASSERT_BKPT) )
#endif

#ifndef vcos_assert_msg
#define vcos_assert_msg(cond, ...) \
   ( (cond) ? (void)0 : (VCOS_ASSERT_MSG(__VA_ARGS__), VCOS_ASSERT_BKPT) )
#endif

#else  /* !defined(NDEBUG) || defined(VCOS_RELEASE_ASSERTS) */

#ifndef vcos_assert
#define vcos_assert(cond) (void)0
#endif

#ifndef vcos_assert_msg
#define vcos_assert_msg(cond, ...) (void)0
#endif

#endif /* !defined(NDEBUG) || defined(VCOS_RELEASE_ASSERTS) */

#if !defined(NDEBUG)

#ifndef vcos_demand
#define vcos_demand(cond) \
   ( (cond) ? (void)0 : (VCOS_ASSERT_MSG("%s", #cond), VCOS_ASSERT_BKPT, vcos_abort()) )
#endif

#ifndef vcos_demand_msg
#define vcos_demand_msg(cond, ...) \
   ( (cond) ? (void)0 : (VCOS_ASSERT_MSG(__VA_ARGS__), VCOS_ASSERT_BKPT, vcos_abort()) )
#endif

#ifndef vcos_verify
#define vcos_verify(cond) \
   ( (cond) ? 1 : (VCOS_VERIFY_MSG("%s", #cond), VCOS_VERIFY_BKPT, 0) )
#endif

#ifndef vcos_verify_msg
#define vcos_verify_msg(cond, ...) \
   ( (cond) ? 1 : (VCOS_VERIFY_MSG(__VA_ARGS__), VCOS_VERIFY_BKPT, 0) )
#endif

#else  /* !defined(NDEBUG) */

#ifndef vcos_demand
#define vcos_demand(cond) \
   ( (cond) ? (void)0 : vcos_abort() )
#endif

#ifndef vcos_demand_msg
#define vcos_demand_msg(cond, ...) \
   ( (cond) ? (void)0 : vcos_abort() )
#endif

#ifndef vcos_verify
#define vcos_verify(cond) (cond)
#endif

#ifndef vcos_verify_msg
#define vcos_verify_msg(cond, ...) (cond)
#endif

#endif /* !defined(NDEBUG) */

#ifndef vcos_static_assert
#if defined(__GNUC__)
#define vcos_static_assert(cond) __attribute__((unused)) extern int vcos_static_assert[(cond)?1:-1]
#else
#define vcos_static_assert(cond) extern int vcos_static_assert[(cond)?1:-1]
#endif
#endif

#ifndef vc_assert
#define vc_assert(cond) vcos_assert(cond)
#endif

/** Print out a backtrace, on supported platforms.
  */
extern void vcos_backtrace_self(void);

#ifdef __cplusplus
}
#endif

#endif /* VCOS_ASSERT_H */
