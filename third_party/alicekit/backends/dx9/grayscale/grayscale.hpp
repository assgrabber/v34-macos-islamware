#pragma once

#include <grayscale/grayscale.hpp>

#include <d3d9.h>

namespace alice
{
    // Direct3D 9 desaturation for disabled widgets.  Works directly on the
    // ImGui vertex buffer, so it needs no extra render targets.
    class c_dx9_grayscale final : public c_grayscale
    {
    public:
        c_dx9_grayscale( IDirect3DDevice9* device ) : device( device ) { }

        void resize( int, int ) override { }

        void apply( ImDrawList* dl, int vtx_start, int vtx_end, int, int, float strength ) override;

        void add_image( ImDrawList* dl, ImTextureID tex, ImVec2 p_min, ImVec2 p_max,
                        float, ImU32 col = IM_COL32_WHITE ) override
        {
            dl->AddImage( tex, p_min, p_max, ImVec2( 0, 0 ), ImVec2( 1, 1 ), col );
        }

        void add_image_rounded( ImDrawList* dl, ImTextureID tex, ImVec2 p_min, ImVec2 p_max,
                                float rounding, ImDrawFlags flags, float, ImU32 col = IM_COL32_WHITE ) override
        {
            dl->AddImageRounded( tex, p_min, p_max, ImVec2( 0, 0 ), ImVec2( 1, 1 ), col, rounding, flags );
        }

    private:
        IDirect3DDevice9* device = nullptr;
    };
} // namespace alice
