#pragma once

// Minimal logger shim so alicekit builds without the original logger module.
// Messages go to stderr, which the launcher captures in its log file.

#include <cstdio>

namespace alice
{
    template < typename... Args >
    inline void log_info( char const*, Args&&... )
    {
    }

    template < typename... Args >
    inline void log_success( char const*, Args&&... )
    {
    }

    template < typename... Args >
    inline void log_err( char const* fmt, Args&&... )
    {
        fprintf( stderr, "[alice][err] %s\n", fmt );
    }

    template < typename... Args >
    inline void log_warn( char const* fmt, Args&&... )
    {
        fprintf( stderr, "[alice][warn] %s\n", fmt );
    }
} // namespace alice
