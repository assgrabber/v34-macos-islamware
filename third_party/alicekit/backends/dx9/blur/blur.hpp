#pragma once

#include <blur/blur.hpp>

#include <d3d9.h>

#include <vector>

namespace alice
{
    // Direct3D 9 implementation of alicekit's blur.  There is no shader
    // compiler dependency: the blur is a progressive bilinear downsample /
    // blended upsample pyramid, which approximates a gaussian closely enough
    // for a menu backdrop and never touches the game's textures.
    class c_dx9_blur final : public c_blur
    {
    public:
        c_dx9_blur( IDirect3DDevice9* device, int width, int height );
        ~c_dx9_blur( ) override;

        void resize( int w, int h ) override;

        bool init_duplication( ) override { return true; }
        void capture_desktop( ) override { capture( ); }

        void prepare_frame( float radius = 12.f, int passes = 2 ) override;
        void begin( ImDrawList* dl, float radius = 12.f, int passes = 2 ) override;

        void apply( ImDrawList* dl, ImVec2 rect_min, ImVec2 rect_max,
                    float radius = 12.f, float rounding = 0.f, int passes = 2, float alpha = 1.f ) override;
        void apply_squircle( ImDrawList* dl, ImVec2 rect_min, ImVec2 rect_max,
                             float radius = 12.f, float rounding = 0.f,
                             float exponent = 5.f, float arm_ratio = 1.f,
                             int passes = 2, float alpha = 1.f ) override;

        [[nodiscard]] ImTextureID get_texture( ) const override;
        void uv_for( ImVec2 p_min, ImVec2 p_max, ImVec2& uv_min, ImVec2& uv_max ) const override;
        void add_image( ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, ImU32 col = IM_COL32_WHITE ) const override;
        void add_image_rounded( ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float rounding,
                                ImDrawFlags flags = ImDrawFlags_RoundCornersAll, ImU32 col = IM_COL32_WHITE ) const override;
        void add_image_squircle( ImDrawList* dl, ImVec2 p_min, ImVec2 p_max, float rounding,
                                 ImDrawFlags flags = ImDrawFlags_RoundCornersAll,
                                 float exponent = 5.f, float arm_ratio = 1.f,
                                 int segs_per_corner = 0, ImU32 col = IM_COL32_WHITE ) const override;

    private:
        struct level_t
        {
            IDirect3DTexture9* tex = nullptr;
            IDirect3DSurface9* surf = nullptr;
            int w = 0;
            int h = 0;
        };

        void release( );
        void create_levels( int w, int h );
        void capture( );
        void blend_level( level_t& dst, IDirect3DTexture9* src );

        IDirect3DDevice9* device = nullptr;
        int width = 0;
        int height = 0;
        std::vector< level_t > levels;
        int last_frame = -1;
        bool captured_this_frame = false;
    };
} // namespace alice
