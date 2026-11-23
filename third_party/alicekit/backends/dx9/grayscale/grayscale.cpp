#include "grayscale.hpp"

#include "imgui.h"

#include <algorithm>

using alice::c_dx9_grayscale;

void c_dx9_grayscale::apply( ImDrawList* dl, int vtx_start, int vtx_end, int, int, float strength )
{
    if ( !dl || strength <= 0.001f )
        return;

    strength = std::clamp( strength, 0.f, 1.f );

    int const count = std::min( vtx_end, dl->VtxBuffer.Size );
    for ( int i = std::max( 0, vtx_start ); i < count; i++ )
    {
        ImDrawVert& v = dl->VtxBuffer[ i ];
        int const a = ( int )( ( v.col >> IM_COL32_A_SHIFT ) & 0xFF );
        int const r = ( int )( ( v.col >> IM_COL32_R_SHIFT ) & 0xFF );
        int const g = ( int )( ( v.col >> IM_COL32_G_SHIFT ) & 0xFF );
        int const b = ( int )( ( v.col >> IM_COL32_B_SHIFT ) & 0xFF );
        int const lum = ( r * 299 + g * 587 + b * 114 ) / 1000;

        int const nr = ( int )( r + ( lum - r ) * strength );
        int const ng = ( int )( g + ( lum - g ) * strength );
        int const nb = ( int )( b + ( lum - b ) * strength );

        v.col = IM_COL32( nr, ng, nb, a );
    }
}
