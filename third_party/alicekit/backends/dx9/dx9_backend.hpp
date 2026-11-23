#pragma once

#include "backends/backend.hpp"

#include <d3d9.h>

namespace alice
{
    class c_dx9_backend final : public i_backend
    {
    public:
        c_dx9_backend( IDirect3DDevice9* device, int width, int height );

        std::unique_ptr< c_blur > make_blur( ) override;
        std::unique_ptr< c_glow > make_glow( ) override;
        std::unique_ptr< c_grayscale > make_grayscale( ) override;
        std::unique_ptr< c_model_engine > make_model_engine( ) override;

        // Input is A8R8G8B8 dwords (memory order B,G,R,A), which matches both
        // the packed font atlas and textures produced by our PNG loader.
        ImTextureID create_texture_rgba( void const* rgba, int width, int height ) override;
        void destroy_texture( ImTextureID id ) override;

        void set_size( int w, int h );

    private:
        IDirect3DDevice9* device = nullptr;
        int width = 0;
        int height = 0;
    };
} // namespace alice
