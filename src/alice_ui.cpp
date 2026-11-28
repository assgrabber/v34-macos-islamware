#include "alice_ui.h"

#include "cheat.h"

#include "alice_kit.hpp"
#include "assets/assets.hpp"
#include "backends/dx9/dx9_backend.hpp"
#include "fonts/fonts.hpp"
#include "fonts/lucide_defines.hpp"
#include "notifications/notifications.hpp"
#include "themes/themes.hpp"
#include "ui/elem/elem.hpp"
#include "popups/popup_elem/popup_elem.hpp"
#include "smooth_scroll/smooth_scroll.hpp"
#include "ui/ui.hpp"
#include "esp_layout/esp_layout.hpp"
#include "renderer/renderer.hpp"
#include "custom_scrollbar/custom_scrollbar.hpp"

#include "imgui.h"
#include "imgui_internal.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace alice;
using namespace ImGui;

namespace
{
    bool g_initialized = false;
    bool g_open = false;

    ImTextureID load_png( void const* data, size_t size )
    {
        int w = 0, h = 0, channels = 0;
        unsigned char* pixels = stbi_load_from_memory(
            static_cast< stbi_uc const* >( data ), static_cast< int >( size ), &w, &h, &channels, 4 );
        if ( !pixels )
            return 0;

        std::vector< uint32_t > argb( static_cast< size_t >( w ) * h );
        for ( size_t i = 0; i < argb.size( ); i++ )
        {
            uint8_t const r = pixels[ i * 4 + 0 ];
            uint8_t const g = pixels[ i * 4 + 1 ];
            uint8_t const b = pixels[ i * 4 + 2 ];
            uint8_t const a = pixels[ i * 4 + 3 ];
            argb[ i ] = ( ( uint32_t ) a << 24 ) | ( ( uint32_t ) r << 16 ) | ( ( uint32_t ) g << 8 ) | b;
        }

        stbi_image_free( pixels );

        if ( !core || !core->backend )
            return 0;
        return core->backend->create_texture_rgba( argb.data( ), w, h );
    }

    // ------------------------------------------------------------------
    // keybinds
    // ------------------------------------------------------------------
    struct bind_state_t
    {
        int* target = nullptr;
        int* mode = nullptr;
        bool wait_release = false;
        ImVec2 pos { };
    };
    static bind_state_t g_bind;
    static bool g_bind_capture = false;
    static bool g_bind_request = false;

    static bool vk_down( int vk )
    {
        return vk > 0 && ( GetAsyncKeyState( vk ) & 0x8000 ) != 0;
    }

    static const char* vk_name( int vk )
    {
        switch ( vk )
        {
        case 0: return "-";
        case VK_LBUTTON: return "LMB";
        case VK_RBUTTON: return "RMB";
        case VK_MBUTTON: return "MMB";
        case VK_XBUTTON1: return "M4";
        case VK_XBUTTON2: return "M5";
        case VK_SPACE: return "SPACE";
        case VK_TAB: return "TAB";
        case VK_SHIFT: return "SHIFT";
        case VK_CONTROL: return "CTRL";
        case VK_MENU: return "ALT";
        case VK_INSERT: return "INS";
        case VK_DELETE: return "DEL";
        case VK_HOME: return "HOME";
        case VK_END: return "END";
        case VK_PRIOR: return "PGUP";
        case VK_NEXT: return "PGDN";
        case VK_UP: return "UP";
        case VK_DOWN: return "DOWN";
        case VK_LEFT: return "LEFT";
        case VK_RIGHT: return "RIGHT";
        case VK_RETURN: return "ENTER";
        case VK_ESCAPE: return "ESC";
        case VK_CAPITAL: return "CAPS";
        default: break;
        }

        static char buf[ 16 ];
        if ( vk >= VK_F1 && vk <= VK_F12 )
            snprintf( buf, sizeof( buf ), "F%d", vk - VK_F1 + 1 );
        else if ( vk >= 'A' && vk <= 'Z' )
            snprintf( buf, sizeof( buf ), "%c", vk );
        else if ( vk >= '0' && vk <= '9' )
            snprintf( buf, sizeof( buf ), "%c", vk );
        else
            snprintf( buf, sizeof( buf ), "VK%02X", vk );
        return buf;
    }

    static bool g_bind_window_open = false;
    static bool g_bind_capturing = false;

    struct bind_hover_t
    {
        ImVec4 bg { 1.f / 255.f, 1.f / 255.f, 1.f / 255.f, 1.f };
        ImVec4 border { 3.f / 255.f, 3.f / 255.f, 3.f / 255.f, 1.f };
    };

    static void draw_keybind_popup( )
    {
        g_bind_capture = g_bind_window_open;
        if ( !cfg.menu_open || !g_bind_window_open || !g_bind.target )
            return;

        ImGuiIO& io = ImGui::GetIO( );
        float const W = 280.f;
        float const H = g_bind_capturing ? 84.f : 150.f;

        ImVec2 pos = g_bind.pos;
        pos.x = ImClamp( pos.x, 8.f, ImMax( 8.f, io.DisplaySize.x - W - 8.f ) );
        pos.y = ImClamp( pos.y, 8.f, ImMax( 8.f, io.DisplaySize.y - H - 8.f ) );

        ImGui::SetNextWindowPos( pos, ImGuiCond_Always );
        ImGui::SetNextWindowSize( { W, H }, ImGuiCond_Always );
        ImGui::PushStyleColor( ImGuiCol_WindowBg, ImVec4( 2.f / 255.f, 2.f / 255.f, 2.f / 255.f, 1.f ) );
        ImGui::PushStyleColor( ImGuiCol_Border, ImVec4( 10.f / 255.f, 10.f / 255.f, 10.f / 255.f, 1.f ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 0, 0 ) );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowRounding, 8.f );
        ImGui::PushStyleVar( ImGuiStyleVar_WindowBorderSize, 1.f );

        bool const open = ImGui::Begin( "##evv_bindwin", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav );

        if ( !open )
        {
            g_bind_window_open = false;
            g_bind_capturing = false;
            g_bind_capture = false;
            ImGui::End( );
            ImGui::PopStyleVar( 3 );
            ImGui::PopStyleColor( 2 );
            return;
        }

        ImGuiWindow* win = ImGui::GetCurrentWindow( );
        ImDrawList* dl = win->DrawList;
        auto const& t = c_themes::palette( );
        float const anim_speed = io.DeltaTime * 20.f;

        // Draggable header.
        ImGui::SetCursorScreenPos( win->Pos );
        ImGui::InvisibleButton( "##bind_drag", { W, 26.f } );
        if ( ImGui::IsItemActive( ) )
        {
            g_bind.pos.x += io.MouseDelta.x;
            g_bind.pos.y += io.MouseDelta.y;
        }

        dl->AddText( { win->Pos.x + 12.f, win->Pos.y + 7.f },
                     color_t( t.text_muted ).modulate( ), "KEYBIND" );

        auto field = [ & ]( char const* id, ImVec2 const& p, ImVec2 const& sz,
                            char const* text, ImVec4 const& col ) -> bool {
            ImGui::SetCursorScreenPos( p );
            bool const pressed = ImGui::InvisibleButton( id, sz );
            bool const hovered = ImGui::IsItemHovered( );

            ImGuiID const fid = ImGui::GetID( id );
            auto* anim = c_elem_animations::instance( ).get< bind_hover_t >( win->Name, fid );
            ImVec4 const bg_target = hovered ? ImVec4( 0.035f, 0.035f, 0.035f, 1.f )
                                             : ImVec4( 1.f / 255.f, 1.f / 255.f, 1.f / 255.f, 1.f );
            ImVec4 const bd_target = hovered ? ImVec4( 0.10f, 0.10f, 0.10f, 1.f )
                                             : ImVec4( 3.f / 255.f, 3.f / 255.f, 3.f / 255.f, 1.f );
            anim->bg = ImLerp( anim->bg, bg_target, anim_speed );
            anim->border = ImLerp( anim->border, bd_target, anim_speed );

            dl->AddRectFilled( p, p + sz, color_t( anim->bg ).modulate( ), 6.f );
            dl->AddRect( p, p + sz, color_t( anim->border ).modulate( ), 6.f );
            ImVec2 const ts = ImGui::CalcTextSize( text );
            dl->AddText( { p.x + ( sz.x - ts.x ) * 0.5f, p.y + ( sz.y - ts.y ) * 0.5f },
                         color_t( col ).modulate( ), text );
            return pressed;
        };

        if ( g_bind_capturing )
        {
            char const* msg = "Press a key to bind...";
            ImVec2 const ms = ImGui::CalcTextSize( msg );
            dl->AddText( { win->Pos.x + ( W - ms.x ) * 0.5f, win->Pos.y + 30.f },
                         color_t( t.text ).modulate( ), msg );

            char const* esc = "Esc to cancel";
            ImVec2 const es = ImGui::CalcTextSize( esc );
            dl->AddText( { win->Pos.x + ( W - es.x ) * 0.5f, win->Pos.y + 50.f },
                         color_t( t.text_hint ).modulate( ), esc );

            if ( GetAsyncKeyState( VK_ESCAPE ) & 0x8000 )
            {
                g_bind_capturing = false;
            }
            else
            {
                for ( int vk = 1; vk < 256; vk++ )
                {
                    if ( vk == VK_LBUTTON || vk == VK_RBUTTON )
                        continue;
                    if ( GetAsyncKeyState( vk ) & 0x8000 )
                    {
                        *g_bind.target = vk;
                        g_bind_capturing = false;
                        break;
                    }
                }
            }
        }
        else
        {
            float const x = win->Pos.x + 12.f;
            float const y = win->Pos.y + 32.f;
            float const key_w = 128.f;
            float const mode_w = 80.f;
            float const del_w = 30.f;
            float const gap = 6.f;

            char const* key_text = *g_bind.target ? vk_name( *g_bind.target ) : "-";
            if ( field( "##bind_key", { x, y }, { key_w, 28.f }, key_text, t.text ) )
                g_bind_capturing = true;

            if ( g_bind.mode )
            {
                char const* mode_text = *g_bind.mode == 0 ? "Hold" : "Toggle";
                if ( field( "##bind_mode", { x + key_w + gap, y }, { mode_w, 28.f }, mode_text, t.text_muted ) )
                    *g_bind.mode = *g_bind.mode == 0 ? 1 : 0;
            }

            float const del_x = x + key_w + gap + ( g_bind.mode ? mode_w + gap : 0.f );
            ImGui::SetCursorScreenPos( { del_x, y } );
            bool const del_pressed = ImGui::InvisibleButton( "##bind_del", { del_w, 28.f } );
            bool const del_hovered = ImGui::IsItemHovered( );
            dl->AddRectFilled( { del_x, y }, { del_x + del_w, y + 28.f }, color_t( 1, 1, 1 ).modulate( ), 6.f );
            dl->AddRect( { del_x, y }, { del_x + del_w, y + 28.f },
                         color_t( del_hovered ? 45 : 3, del_hovered ? 12 : 3, del_hovered ? 12 : 3 ).modulate( ), 6.f );
            char const* tr = ICON_LUCIDE_TRASH_2;
            ImVec2 const ts = ImGui::CalcTextSize( tr );
            dl->AddText( { del_x + ( del_w - ts.x ) * 0.5f, y + ( 28.f - ts.y ) * 0.5f },
                         color_t( del_hovered ? t.danger : t.text_muted ).modulate( ), tr );
            if ( del_pressed )
                *g_bind.target = 0;

            float const add_y = y + 28.f + 10.f;
            if ( field( "##bind_add", { x, add_y }, { W - 24.f, 30.f }, "Add keybind", t.text ) )
                g_bind_capturing = true;

            float const close_y = add_y + 30.f + 8.f;
            if ( field( "##bind_close", { x, close_y }, { W - 24.f, 30.f }, "Close", t.text_muted ) )
            {
                g_bind_window_open = false;
                g_bind_capture = false;
            }
        }

        ImGui::End( );
        ImGui::PopStyleVar( 3 );
        ImGui::PopStyleColor( 2 );
    }

    static void open_bind_window( int* key, int* mode, ImVec2 const& pos )
    {
        g_bind.target = key;
        g_bind.mode = mode;

        ImVec2 p = pos;
        if ( ImGuiWindow* menu = ImGui::FindWindowByName( "##islamware" ) )
        {
            p.x = menu->Pos.x + menu->Size.x + 12.f;
            p.y = pos.y - 16.f;
        }
        g_bind.pos = p;
        g_bind_window_open = true;
        g_bind_capturing = false;
        g_bind_capture = true;
    }

    static void bind_row( const char* label, bool* value, int* key, int* mode = nullptr,
                          ImVec4* color = nullptr, ImVec4* color2 = nullptr,
                          const char* tip = nullptr );

    static void bind_row_ex( const char* label, bool* value, int id,
                             ImVec4* color = nullptr, ImVec4* color2 = nullptr )
    {
        bind_row( label, value, &cfg.kb_feature[ id ], &cfg.kb_feature_mode[ id ], color, color2 );
    }

    static void bind_row( const char* label, bool* value, int* key, int* mode,
                          ImVec4* color, ImVec4* color2, const char* tip )
    {
        auto it = c_ui_elements::start( label );
        ImRect const row = it.rect;
        it.add_toggle( value );
        if ( color2 )
            it.add_color( std::pair< ImVec4*, std::optional< ImVec4* > >{ color, std::make_optional( color2 ) } );
        else if ( color )
            it.add_color( color );
        // notes show only after hovering the function name for a second
        if ( tip ) it.add_tooltip( tip );
        it.finish( );

        auto const& t = c_themes::palette( );

        if ( key && *key )
        {
            ImVec2 const label_sz = ImGui::CalcTextSize( label );
            ImVec2 const dot = { row.Min.x + 15.0f + label_sz.x + 7.0f, row.GetCenter( ).y };
            ImGui::GetWindowDrawList( )->AddCircleFilled( dot, 2.6f,
                color_t( t.accent ).modulate( ImGui::GetStyle( ).Alpha ), 16 );

            const char* n = vk_name( *key );
            ImVec2 const sz = ImGui::CalcTextSize( n );
            ImVec2 const p = { it.rect.Max.x - sz.x - 6.0f, row.GetCenter( ).y - sz.y * 0.5f };
            ImGui::GetWindowDrawList( )->AddText( p, IM_COL32( 150, 152, 168, 255 ), n );
        }

        if ( key && ImGui::IsMouseHoveringRect( row.Min, row.Max ) && ImGui::IsMouseClicked( ImGuiMouseButton_Right ) )
            open_bind_window( key, mode, { row.Min.x, row.Max.y + 4.0f } );
    }

    // Inline bind field for the settings page (click the box, press a key).
    static void bind_input_row( const char* label, int* key, int* mode = nullptr )
    {
        auto it = c_ui_elements::start( label );
        ImRect const row = it.rect;
        it.finish( );

        auto const& t = c_themes::palette( );
        float const w = 130.0f;
        float const h = 26.0f;
        ImRect const box = {
            { row.Max.x - w, row.GetCenter( ).y - h * 0.5f },
            { row.Max.x, row.GetCenter( ).y + h * 0.5f } };

        ImDrawList* dl = ImGui::GetWindowDrawList( );
        dl->AddRectFilled( box.Min, box.Max, color_t( 1, 1, 1 ).modulate( ), 6.f );
        dl->AddRect( box.Min, box.Max, color_t( 3, 3, 3 ).modulate( ), 6.f );

        char const* name = key && *key ? vk_name( *key ) : "-";
        ImVec2 const sz = ImGui::CalcTextSize( name );
        dl->AddText( { box.GetCenter( ).x - sz.x * 0.5f, box.GetCenter( ).y - sz.y * 0.5f },
                     color_t( t.text ).modulate( ), name );

        bool const hovered = ImGui::IsMouseHoveringRect( box.Min, box.Max );
        if ( hovered && ( ImGui::IsMouseClicked( ImGuiMouseButton_Left ) ||
                          ImGui::IsMouseClicked( ImGuiMouseButton_Right ) ) )
            open_bind_window( key, mode, { box.Min.x, box.Max.y + 4.0f } );
    }

    // ------------------------------------------------------------------
    // configs
    // ------------------------------------------------------------------
    static std::string g_config_dir;
    static std::vector< std::string > g_configs;
    static int g_config_sel = -1;
    static char g_config_name[ 64 ] = "default";
    static bool g_config_ready = false;

    static void config_refresh( )
    {
        g_configs.clear( );
        WIN32_FIND_DATAA fd { };
        std::string pattern = g_config_dir + "\\*.bin";
        HANDLE h = FindFirstFileA( pattern.c_str( ), &fd );
        if ( h != INVALID_HANDLE_VALUE )
        {
            do
            {
                std::string n = fd.cFileName;
                size_t dot = n.rfind( ".bin" );
                if ( dot != std::string::npos )
                    g_configs.push_back( n.substr( 0, dot ) );
            } while ( FindNextFileA( h, &fd ) );
            FindClose( h );
        }
        std::sort( g_configs.begin( ), g_configs.end( ) );
        if ( g_config_sel >= ( int ) g_configs.size( ) )
            g_config_sel = g_configs.empty( ) ? -1 : ( int ) g_configs.size( ) - 1;
        if ( g_config_sel < 0 && !g_configs.empty( ) )
            g_config_sel = 0;
    }

    static void config_init( )
    {
        if ( g_config_ready )
            return;
        g_config_ready = true;

        char exe[ MAX_PATH ] = { 0 };
        if ( !GetModuleFileNameA( nullptr, exe, MAX_PATH ) )
            return;
        char* slash = strrchr( exe, '\\' );
        if ( slash )
            *slash = '\0';
        g_config_dir = std::string( exe ) + "\\islamware_configs";
        CreateDirectoryA( g_config_dir.c_str( ), nullptr );
        config_refresh( );
    }

    static void config_save( const std::string& name )
    {
        if ( name.empty( ) || g_config_dir.empty( ) )
            return;
        std::string path = g_config_dir + "\\" + name + ".bin";
        FILE* f = fopen( path.c_str( ), "wb" );
        if ( !f )
            return;
        unsigned int magic = 0x45565743; // 'EVWC'
        unsigned int ver = 7; // 7 = first-person weapon chams off (tinted the arms)
        unsigned int size = ( unsigned int ) sizeof( Config );
        fwrite( &magic, 4, 1, f );
        fwrite( &ver, 4, 1, f );
        fwrite( &size, 4, 1, f );
        fwrite( &cfg, sizeof( Config ), 1, f );
        fclose( f );
        config_refresh( );
    }

    static void config_load( const std::string& name )
    {
        if ( name.empty( ) || g_config_dir.empty( ) )
            return;
        std::string path = g_config_dir + "\\" + name + ".bin";
        FILE* f = fopen( path.c_str( ), "rb" );
        if ( !f )
            return;
        unsigned int magic = 0, ver = 0, size = 0;
        fread( &magic, 4, 1, f );
        fread( &ver, 4, 1, f );
        fread( &size, 4, 1, f );
        if ( magic == 0x45565743 ) {
            // Config is dumped as a raw struct; any field added since the file
            // was saved shifts later fields.  A partial read can leave a char
            // array without its terminator and the next frame crashes inside
            // the CRT (ucrtbase) on strlen/model lookup.  Only accept exact
            // layout matches.
            if ( size > sizeof( Config ) ) {
                Log( "config load: %s ignored, layout changed (%u bytes on disk, %u now) - re-save the config",
                     name.c_str( ), size, (unsigned)sizeof( Config ) );
            } else {
                fseek( f, 12, SEEK_SET );
                bool const partial = ( ver <= 3 ) && ( size < sizeof( Config ) );
                // loading a config must not touch the live menu state: the raw
                // dump was saved with the menu closed, which made the Load
                // button close the menu
                bool const menu_was_open = cfg.menu_open;
                bool const menu_was_imgui = cfg.menu_imgui;
                if ( partial )
                    memset( &cfg, 0, sizeof( Config ) );
                if ( fread( &cfg, 1, size, f ) == size ) {
                    // defensive: raw dumps can never leave strings unterminated
                    cfg.model_path[ sizeof( cfg.model_path ) - 1 ] = 0;
                    cfg.reported_build[ sizeof( cfg.reported_build ) - 1 ] = 0;
                    cfg.menu_open = menu_was_open;     // keep the menu open
                    cfg.menu_imgui = menu_was_imgui;   // keep the active style
                    if ( partial ) {
                        // older layouts end before the tail fields; the raw
                        // copy shifted them, so restore known-good values
                        cfg.menu_imgui = true;      // ImGui menu
                        cfg.browser_fix = true;
                        cfg.connect_fix = true;
                        cfg.spoof_version = true;
                        strcpy( cfg.reported_build, "11003710" );
                        cfg.allow_internet = true;
                        cfg.esp_layout[0] = 0;
                        cfg.hotkey_list = true;
                        cfg.hotkey_x = 24.0f;
                        cfg.hotkey_y = 140.0f;
                        cfg.bomb_preview = true;
                    }
                    // ver 1 configs were saved with the old 8-entry chams
                    // dropdown whose indices were shifted by one vs the
                    // material table - remap them to the labels' intent
                    // chams material tables changed twice; remap saved
                    // selections to the current 4-type table
                    //   ver 1: 8 entries (Regular/Flat/Shaded/Metallic/Glow/Glass/Wire/Scroll)
                    //   ver 2: 7 entries (Flat/Shaded/Metallic/Glow/Glass/Wire/Scroll)
                    //   ver 3: 4 entries (Flat/Shaded/Glass/Wireframe)
                    static const int map_v1[] = { 0, 0, 1, 1, 1, 2, 3, 0 };
                    static const int map_v2[] = { 0, 1, 1, 1, 2, 3, 0 };
                    if ( ver == 1 || ver == 2 ) {
                        int const* map = ( ver == 1 ) ? map_v1 : map_v2;
                        Config::ChamsTarget* ts[] = { &cfg.chams_enemy, &cfg.chams_team,
                            &cfg.chams_local, &cfg.chams_viewmodel, &cfg.chams_weapons };
                        for ( Config::ChamsTarget* t : ts ) {
                            Config::ChamsLayer* ls[] = { &t->occluded, &t->visible, &t->overlay };
                            for ( Config::ChamsLayer* l : ls ) {
                                int const m = l->material;
                                l->material = ( m >= 0 && m <= 7 ) ? map[m] : 0;
                            }
                        }
                        int const hm = cfg.chams_hit_material;
                        cfg.chams_hit_material = ( hm >= 0 && hm <= 7 ) ? map[hm] : 0;
                    }
                    if ( ver <= 4 ) {
                        // ver 1-4 saved the 4-material table (Flat/Shaded/Glass/
                        // Wireframe); map to the 7-material table
                        static const int map_v4[] = { 1, 2, 5, 6 };
                        Config::ChamsTarget* ts[] = { &cfg.chams_enemy, &cfg.chams_team,
                            &cfg.chams_local, &cfg.chams_viewmodel, &cfg.chams_weapons };
                        for ( Config::ChamsTarget* t : ts ) {
                            Config::ChamsLayer* ls[] = { &t->occluded, &t->visible, &t->overlay };
                            for ( Config::ChamsLayer* l : ls ) {
                                int const m = l->material;
                                l->material = ( m >= 0 && m <= 3 ) ? map_v4[m] : 1;
                            }
                        }
                        int const hm = cfg.chams_hit_material;
                        cfg.chams_hit_material = ( hm >= 0 && hm <= 3 ) ? map_v4[hm] : 6;
                    }
                    if ( ver < 6 ) {
                        // ver 5 and older saved the light-blue armor bottom
                        // colour; the reference gradient is dark blue
                        static const float armor_lo[4] = { 0.2f, 0.55f, 1.0f, 1.0f };
                        static const float armor_hi[4] = { 0.05f, 0.15f, 0.45f, 1.0f };
                        memcpy( cfg.esp_armor_min, armor_lo, sizeof( armor_lo ) );
                        memcpy( cfg.esp_armor_max, armor_hi, sizeof( armor_hi ) );
                    }
                    if ( ver < 7 ) {
                        // ver 6 and older defaulted "First person weapon" on;
                        // v_ models contain the arms, so it recoloured the
                        // hands.  Keep it off unless the user opts in again.
                    }
                    esp_layout::ReloadFromConfig( );
                    Log( "config load: %s ver=%u bytes=%u ok", name.c_str( ), ver, size );
                } else {
                    Log( "config load: %s short read", name.c_str( ) );
                }
            }
        } else {
            Log( "config load failed: %s magic=%08X", name.c_str( ), magic );
        }
        fclose( f );
    }

    static void config_delete( const std::string& name )
    {
        if ( name.empty( ) || g_config_dir.empty( ) )
            return;
        std::string path = g_config_dir + "\\" + name + ".bin";
        remove( path.c_str( ) );
        config_refresh( );
    }

    // ------------------------------------------------------------------
    // shared option lists
    // ------------------------------------------------------------------
    namespace lists
    {
        std::vector< std::string > hitboxes( ) { return { "Head", "Neck", "Chest", "Stomach", "Pelvis" }; }
        std::vector< std::string > targets( ) { return { "Crosshair", "Distance", "Health" }; }
        std::vector< std::string > aim_keys( ) { return { "Always", "Mouse4", "Mouse5", "Alt" }; }
        std::vector< std::string > aa_pitch( ) { return { "None", "Down", "Up", "Zero" }; }
        std::vector< std::string > aa_base( ) { return { "At Local", "At Target" }; }
        std::vector< std::string > aa_yaw( ) { return { "Forward", "Backward" }; }
        std::vector< std::string > aa_mod( ) { return { "None", "Center", "Offset", "Random", "Spin", "Three-way", "Five-way" }; }
        std::vector< std::string > strafe_modes( ) { return { "Follow camera", "Follow key" }; }
        std::vector< std::string > chams_types( ) { return { "Flat", "Shaded", "Glass", "Wireframe" }; }
        std::vector< std::string > chams_targets( ) { return { "Enemies", "Teammates", "Local", "Viewmodel", "Weapons" }; }
        std::vector< std::string > kill_effects( ) { return { "Tesla", "Explosion", "Blood", "Gauss" }; }
    }

    static void hitbox_multiselect( bool* values[ 5 ] )
    {
        auto it = c_ui_elements::start( "Hitboxes" );
        ImRect const row = it.rect;
        it.finish( );

        char const* names[ 5 ] = { "Head", "Neck", "Chest", "Stomach", "Pelvis" };
        char preview[ 64 ] = { 0 };
        for ( int i = 0; i < 5; i++ )
            if ( values[ i ] && *values[ i ] ) {
                if ( preview[0] ) strncat( preview, ", ", sizeof( preview ) - strlen( preview ) - 1 );
                strncat( preview, names[ i ], sizeof( preview ) - strlen( preview ) - 1 );
            }
        if ( !preview[0] ) strncpy( preview, "None", sizeof( preview ) - 1 );

        ImGuiWindow* window = GetCurrentWindow( );
        ImGuiID const id = window->GetID( "##hitboxes" );
        auto* info = c_elem_animations::instance( ).get< dropdown_info_t >( window->Name, id );
        if ( info->random_title.empty( ) )
            info->random_title = "hitboxes_dd";

        ImRect const rect = {
            { row.Max.x - 150.0f, row.GetCenter( ).y - 11.0f },
            { row.Max.x, row.GetCenter( ).y + 11.0f } };

        if ( std_elem::begin_dropdown( id, rect, info, preview, 5 ) )
        {
            for ( int i = 0; i < 5; i++ )
            {
                PushID( i );
                bool const on = values[ i ] && *values[ i ];
                if ( c_popup_elem::selectable( std::nullopt, names[ i ], on ) && values[ i ] )
                    *values[ i ] = !*values[ i ];
                PopID( );
            }
            c_smooth_scroll::instance( ).pop_step( );
            info->popup.end( );
        }
    }

    // ------------------------------------------------------------------
    // Ragebot
    // ------------------------------------------------------------------
    class rage_aim_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Aim"; }
        std::string icon( ) const override { return ICON_LUCIDE_CROSSHAIR; }

        void render( ) override
        {
            static int weapon_sel = 0;
            std::vector< std::string > weapons = { "Global", "Rifle", "Sniper", "Pistol", "SMG", "Shotgun", "Machine gun", "Knife" };

            c_ui_elements::begin_group( "WEAPON", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Settings for" ).add_dropdown( &weapon_sel, weapons ).finish( );
            Config::WeaponOverride* ov = nullptr;
            if ( weapon_sel > 0 && weapon_sel < ( int ) weapons.size( ) ) {
                ov = &cfg.weapon_override[ weapon_sel - 1 ];
                c_ui_elements::start( "Override Global" ).add_toggle( &ov->enabled ).finish( );
            }
            bool const use_ov = ov && ov->enabled;
            c_ui_elements::end_group( );

            bool* hb[ 5 ] = {
                use_ov ? &ov->hb[0] : &cfg.silent_hb_head,
                use_ov ? &ov->hb[1] : &cfg.silent_hb_neck,
                use_ov ? &ov->hb[2] : &cfg.silent_hb_chest,
                use_ov ? &ov->hb[3] : &cfg.silent_hb_stomach,
                use_ov ? &ov->hb[4] : &cfg.silent_hb_pelvis
            };
            float* sfov = use_ov ? &ov->silent_fov : &cfg.silent_fov;
            float* min_dmg = use_ov ? &ov->min_damage : &cfg.min_damage;
            float* hitchance = use_ov ? &ov->hit_chance : &cfg.hit_chance;
            int* delay = use_ov ? &ov->delay_shot : &cfg.delay_shot;
            bool* autostop = use_ov ? &ov->auto_stop : &cfg.auto_stop;
            bool* autoscope = use_ov ? &ov->auto_scope : &cfg.auto_scope;

            c_ui_elements::begin_group( "AIMBOT", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Silent aim", &cfg.silentaim, &cfg.kb_silentaim, &cfg.kb_mode_silentaim );
            c_ui_elements::start( "Silent FOV (0 = no limit)" ).add_slider( sfov, 0.0f, 180.0f, "%.0f" ).finish( );
            hitbox_multiselect( hb );
            c_ui_elements::start( "Target selection" ).add_dropdown( &cfg.silent_select, lists::targets( ) ).finish( );
            c_ui_elements::start( "Visible only" ).add_toggle( &cfg.silent_visible ).finish( );
            c_ui_elements::start( "Multipoint" ).add_toggle( &cfg.silent_multipoint ).finish( );
            c_ui_elements::start( "Min. damage" ).add_slider( min_dmg, 0.0f, 100.0f, "%.0f" ).finish( );
            c_ui_elements::start( "Hit chance" ).add_slider( hitchance, 0.0f, 100.0f, "%.0f%%" ).finish( );
            c_ui_elements::start( "Delay shot (ms)" ).add_slider( delay, 0, 500, "%d" ).finish( );
            c_ui_elements::start( "Auto stop" ).add_toggle( autostop ).finish( );
            c_ui_elements::start( "Auto scope" ).add_toggle( autoscope ).finish( );
            c_ui_elements::start( "Log misses" ).add_toggle( &cfg.log_misses ).finish( );

            c_ui_elements::start( "Auto wall" ).add_toggle( &cfg.auto_wall ).finish( );
            c_ui_elements::visible_if( "autowall_opts", cfg.auto_wall, [ & ] {
                c_ui_elements::start( "Uses real weapon penetration & surface material" ).finish( );
            } );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "BODY AIM", { GetContentRegionAvail( ).x, 99999.f } );
            {
                std::vector< std::string > baim_modes = { "Default", "Prefer", "Force" };
                std::vector< std::string > baim_hbs = { "Chest", "Stomach", "Pelvis" };
                c_ui_elements::start( "Body aim" ).add_dropdown( &cfg.body_aim, baim_modes ).finish( );
                int hb_ui = cfg.body_aim_hitbox - 2;
                if ( hb_ui < 0 || hb_ui > 2 ) hb_ui = 0;
                c_ui_elements::start( "Body hitbox" ).add_dropdown( &hb_ui, baim_hbs ).finish( );
                cfg.body_aim_hitbox = hb_ui + 2;
                bind_row( "Force body aim", &cfg.baim_force, &cfg.kb_baim, &cfg.kb_mode_baim );
                c_ui_elements::start( "Force if lethal" ).add_toggle( &cfg.baim_lethal ).finish( );
                c_ui_elements::start( "Force in air" ).add_toggle( &cfg.baim_in_air ).finish( );
                c_ui_elements::start( "Force while moving" ).add_toggle( &cfg.baim_moving ).finish( );
                c_ui_elements::start( "Force on peek" ).add_toggle( &cfg.baim_on_peek ).finish( );
                c_ui_elements::start( "Disable if target resolved" ).add_toggle( &cfg.baim_dis_resolved ).finish( );
                c_ui_elements::start( "Disable if target shooting" ).add_toggle( &cfg.baim_dis_shooting ).finish( );
                c_ui_elements::start( "Disable on low damage" ).add_toggle( &cfg.baim_dis_low_dmg ).finish( );
                c_ui_elements::start( "Disable if head safe" ).add_toggle( &cfg.baim_dis_head_safe ).finish( );
            }
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "TRIGGERBOT", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Auto fire", &cfg.triggerbot, &cfg.kb_triggerbot, &cfg.kb_mode_triggerbot,
                      nullptr, nullptr, "Fires inside the Silent FOV (0 = 360)" );
            c_ui_elements::start( "Knifebot" ).add_toggle( &cfg.knifebot ).finish( );
            c_ui_elements::visible_if( "knife_opts", cfg.knifebot, [ & ] {
                c_ui_elements::start( "Knife reach" ).add_slider( &cfg.knife_range, 30.0f, 120.0f, "%.0f" ).finish( );
            } );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "PEEK ASSIST", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Peek assist", &cfg.peek_assist, &cfg.kb_peek_assist, &cfg.kb_mode_peek_assist,
                      reinterpret_cast< ImVec4* >( cfg.peek_assist_color ), nullptr,
                      "Places a circle and walks back on shot or key release" );
            c_ui_elements::visible_if( "peek_assist_opts", cfg.peek_assist, [ & ] {
                bind_input_row( "Peek keybind", &cfg.kb_peek_assist, &cfg.kb_mode_peek_assist );
                c_ui_elements::start( "Retreat on shot" ).add_toggle( &cfg.peek_retreat_on_shot ).finish( );
                c_ui_elements::start( "Retreat on release" ).add_toggle( &cfg.peek_retreat_on_release ).finish( );
                c_ui_elements::start( "Max distance (0 = inf)" ).add_slider( &cfg.peek_max_distance, 0, 200, "%d" ).finish( );
            } );
            c_ui_elements::end_group( );
        }
    };

    class rage_aa_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Anti-aim"; }
        std::string icon( ) const override { return ICON_LUCIDE_GHOST; }

        void render( ) override
        {
            c_ui_elements::begin_group( "ANTI-AIM", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Enable" ).add_toggle( &cfg.aa_enabled ).finish( );
            c_ui_elements::start( "Pitch" ).add_dropdown( &cfg.aa_pitch, lists::aa_pitch( ) ).finish( );
            c_ui_elements::start( "Base" ).add_dropdown( &cfg.aa_base, lists::aa_base( ) ).finish( );
            c_ui_elements::start( "Yaw" ).add_dropdown( &cfg.aa_yaw, lists::aa_yaw( ) ).finish( );
            c_ui_elements::start( "Yaw offset" ).add_slider( &cfg.aa_yaw_offset, -180.0f, 180.0f, "%.0f" ).finish( );
            c_ui_elements::start( "Yaw modifier" ).add_dropdown( &cfg.aa_modifier, lists::aa_mod( ) ).finish( );

            c_ui_elements::visible_if( "modifier_offset", cfg.aa_modifier != 0, [ & ] {
                c_ui_elements::start( "Modifier offset" ).add_slider( &cfg.aa_modifier_offset, 0.0f, 180.0f, "%.0f" ).finish( );
            } );

            c_ui_elements::start( "Yaw sides" ).add_toggle( &cfg.aa_sides ).finish( );

            c_ui_elements::visible_if( "side_opts", cfg.aa_sides, [ & ] {
                c_ui_elements::start( "Left" ).add_slider( &cfg.aa_left, 0.0f, 180.0f, "%.0f" ).finish( );
                c_ui_elements::start( "Right" ).add_slider( &cfg.aa_right, 0.0f, 180.0f, "%.0f" ).finish( );
                c_ui_elements::start( "Side delay" ).add_slider( &cfg.aa_side_delay, 1, 10, "%d" ).finish( );
            } );

            c_ui_elements::start( "Roll" ).add_slider( &cfg.aa_roll, -45.0f, 45.0f, "%.0f" ).finish( );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "MANUALS", { GetContentRegionAvail( ).x, 99999.f } );
            bind_input_row( "Manual left", &cfg.aa_manual_left );
            bind_input_row( "Manual right", &cfg.aa_manual_right );
            c_ui_elements::end_group( );
        }
    };

    class legit_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Legit"; }
        std::string icon( ) const override { return ICON_LUCIDE_MOUSE; }

        void render( ) override
        {
            c_ui_elements::begin_group( "AIMBOT", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Enable aimbot", &cfg.aimbot, &cfg.kb_aimbot, &cfg.kb_mode_aimbot );
            c_ui_elements::start( "Key" ).add_dropdown( &cfg.aim_key, lists::aim_keys( ) ).finish( );
            c_ui_elements::start( "FOV" ).add_slider( &cfg.aim_fov, 1.0f, 60.0f, "%.1f" ).finish( );
            c_ui_elements::start( "Smoothing" ).add_slider( &cfg.aim_smooth, 1.0f, 10.0f, "%.1f" ).finish( );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "TRIGGERBOT", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Enable trigger", &cfg.triggerbot, &cfg.kb_triggerbot, &cfg.kb_mode_triggerbot,
                      nullptr, nullptr, "Fires inside the Silent FOV (0 = 360)" );
            c_ui_elements::end_group( );
        }
    };

    class rage_page : public c_ui_page
    {
    public:
        rage_page( )
        {
            register_subpage< rage_aim_sub >( );
            register_subpage< rage_aa_sub >( );
            register_subpage< legit_sub >( );
        }
        std::string name( ) const override { return "Ragebot"; }
        std::string icon( ) const override { return ICON_LUCIDE_TARGET; }
    };

    // ------------------------------------------------------------------
    // Visuals
    // ------------------------------------------------------------------
    class visuals_esp_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "ESP"; }
        std::string icon( ) const override { return ICON_LUCIDE_EYE; }

        void render( ) override
        {
            if ( ImGui::BeginTable( "##esp_table", 2,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings ) )
            {
                ImGui::TableSetupColumn( "##esp_funcs", ImGuiTableColumnFlags_WidthStretch, 0.58f );
                ImGui::TableSetupColumn( "##esp_prev", ImGuiTableColumnFlags_WidthStretch, 0.42f );
                ImGui::TableNextRow( );
                ImGui::TableNextColumn( );

            // only the left box scrolls; same height as the PREVIEW box, and
            // the scrollbar track is rounded like its grabber
            ImGui::PushStyleVar( ImGuiStyleVar_ScrollbarRounding, 6.0f );
            ImGui::PushStyleColor( ImGuiCol_ScrollbarBg, ImVec4( 0.f, 0.f, 0.f, 0.f ) );
            ImGui::BeginChild( "##esp_left_scroll", ImVec2( 0.f, 420.f ), false );


            // Dedicated compact box for the single ESP master switch, the same
            // way each chams layer gets its own box.
            c_ui_elements::begin_group( "PLAYER ESP", { GetContentRegionAvail( ).x, 71.f }, true );
            bind_row_ex( "Enabled", &cfg.esp, FB_ESP );
            c_ui_elements::end_group( );

            c_ui_elements::visible_if( "esp_elements", cfg.esp, [ & ] {
                c_ui_elements::begin_group( "ELEMENTS", { GetContentRegionAvail( ).x, 99999.f } );
                bind_row_ex( "Box", &cfg.esp_box, FB_ESP_BOX, reinterpret_cast< ImVec4* >( cfg.esp_box_color ) );
                c_ui_elements::visible_if( "esp_box_col", cfg.esp_box, [ & ] {
                    c_ui_elements::start( "Box outline" ).add_color( reinterpret_cast< ImVec4* >( cfg.esp_box_outline ) ).finish( );
                } );
                bind_row_ex( "Name", &cfg.esp_name, FB_ESP_NAME, reinterpret_cast< ImVec4* >( cfg.esp_name_color ) );
                c_ui_elements::start( "Cheat tag" ).add_toggle( &cfg.esp_cheat_tag ).finish( );
                bind_row_ex( "Armor", &cfg.esp_armor, FB_ESP_ARMOR,
                             reinterpret_cast< ImVec4* >( cfg.esp_armor_min ),
                             reinterpret_cast< ImVec4* >( cfg.esp_armor_max ) );
                c_ui_elements::visible_if( "esp_armor_opts", cfg.esp_armor, [ & ] {
                    c_ui_elements::start( "Armor gradient" ).add_toggle( &cfg.esp_armor_gradient ).finish( );
                } );
                bind_row_ex( "Health", &cfg.esp_health, FB_ESP_HEALTH,
                             reinterpret_cast< ImVec4* >( cfg.esp_health_min ),
                             reinterpret_cast< ImVec4* >( cfg.esp_health_max ) );
                c_ui_elements::visible_if( "esp_health_col", cfg.esp_health, [ & ] {
                    c_ui_elements::start( "Health gradient" ).add_toggle( &cfg.esp_health_gradient ).finish( );
                } );
                bind_row_ex( "Weapon", &cfg.esp_weapon, FB_ESP_WEAPON, reinterpret_cast< ImVec4* >( cfg.esp_weapon_color ) );
                c_ui_elements::start( "Weapon icon" ).add_toggle( &cfg.esp_weapon_icon )
                    .add_color( reinterpret_cast< ImVec4* >( cfg.esp_weapon_icon_color ) ).finish( );
                c_ui_elements::start( "C4 icon" ).add_toggle( &cfg.esp_c4_icon )
                    .add_color( reinterpret_cast< ImVec4* >( cfg.esp_c4_icon_color ) ).finish( );
                c_ui_elements::start( "Kit icon" ).add_toggle( &cfg.esp_kit_icon )
                    .add_color( reinterpret_cast< ImVec4* >( cfg.esp_kit_icon_color ) ).finish( );
                c_ui_elements::start( "Taser icon" ).add_toggle( &cfg.esp_taser_icon )
                    .add_color( reinterpret_cast< ImVec4* >( cfg.esp_taser_icon_color ) ).finish( );
                bind_row_ex( "Bomb ESP", &cfg.esp_bomb, FB_ESP_BOMB );
                bind_row_ex( "Ammo", &cfg.esp_ammo, FB_ESP_AMMO,
                             reinterpret_cast< ImVec4* >( cfg.esp_ammo_min ),
                             reinterpret_cast< ImVec4* >( cfg.esp_ammo_max ) );
                c_ui_elements::visible_if( "esp_ammo_opts", cfg.esp_ammo, [ & ] {
                    c_ui_elements::start( "Ammo gradient" ).add_toggle( &cfg.esp_ammo_gradient ).finish( );
                } );
                bind_row_ex( "Flags", &cfg.esp_flags, FB_ESP_FLAGS, reinterpret_cast< ImVec4* >( cfg.esp_flags_color ) );
                c_ui_elements::visible_if( "esp_flag_opts", cfg.esp_flags, [ & ] {
                    c_ui_elements::begin_group( "FLAGS", { GetContentRegionAvail( ).x, 99999.f } );
                    c_ui_elements::start( "Money" ).add_toggle( &cfg.flag_money )
                        .add_color( reinterpret_cast< ImVec4* >( cfg.flag_money_color ) ).finish( );
                    c_ui_elements::start( "Armor flag" ).add_toggle( &cfg.flag_armor )
                        .add_color( reinterpret_cast< ImVec4* >( cfg.flag_armor_color ) ).finish( );
                    c_ui_elements::start( "Kit" ).add_toggle( &cfg.flag_kit )
                        .add_color( reinterpret_cast< ImVec4* >( cfg.flag_kit_color ) ).finish( );
                    c_ui_elements::start( "Defuse" ).add_toggle( &cfg.flag_defuse )
                        .add_color( reinterpret_cast< ImVec4* >( cfg.flag_defuse_color ) ).finish( );
                    c_ui_elements::start( "Flashed" ).add_toggle( &cfg.flag_flashed )
                        .add_color( reinterpret_cast< ImVec4* >( cfg.flag_flashed_color ) ).finish( );
                    c_ui_elements::start( "C4 flag" ).add_toggle( &cfg.flag_c4 )
                        .add_color( reinterpret_cast< ImVec4* >( cfg.flag_c4_color ) ).finish( );
                    c_ui_elements::end_group( );
                } );
                c_ui_elements::start( "Distance" ).add_toggle( &cfg.esp_distance ).finish( );
                bind_row_ex( "Skeleton", &cfg.esp_skeleton, FB_ESP_SKELETON );
                c_ui_elements::start( "Team check" ).add_toggle( &cfg.esp_team_check ).finish( );
                c_ui_elements::end_group( );

                c_ui_elements::begin_group( "DORMANT", { GetContentRegionAvail( ).x, 99999.f } );
                c_ui_elements::start( "Show dormant" ).add_toggle( &cfg.esp_show_dormant ).finish( );
                c_ui_elements::start( "Dormant alpha" ).add_slider( &cfg.esp_dormant_alpha, 0.10f, 1.0f, "%.2f" ).finish( );
                c_ui_elements::start( "Fade dormant" ).add_toggle( &cfg.esp_dormant_fade ).finish( );
                c_ui_elements::visible_if( "esp_fade_time", cfg.esp_dormant_fade, [ & ] {
                    c_ui_elements::start( "Fade time" ).add_slider( &cfg.esp_dormant_fade_time, 1.0f, 60.0f, "%.0fs" ).finish( );
                } );
                c_ui_elements::start( "Grayscale" ).add_slider( &cfg.esp_dormant_grayscale, 0.0f, 1.0f, "%.2f" ).finish( );
                c_ui_elements::end_group( );

                c_ui_elements::begin_group( "OFFSCREEN", { GetContentRegionAvail( ).x, 99999.f } );
                bind_row_ex( "Offscreen arrows", &cfg.esp_offscreen, FB_ESP_OFFSCREEN, reinterpret_cast< ImVec4* >( cfg.esp_offscreen_color ) );
                c_ui_elements::visible_if( "esp_arrow_opts", cfg.esp_offscreen, [ & ] {
                    c_ui_elements::start( "Radius" ).add_slider( &cfg.esp_offscreen_radius, 20.0f, 240.0f, "%.0f" ).finish( );
                    c_ui_elements::start( "Size" ).add_slider( &cfg.esp_offscreen_size, 0.5f, 4.0f, "%.1f" ).finish( );
                } );
                c_ui_elements::end_group( );

                c_ui_elements::begin_group( "AIMBOT", { GetContentRegionAvail( ).x, 99999.f } );
                bind_row_ex( "Visualize aimbot", &cfg.esp_aimbot_visual, FB_ESP_AIMBOT, reinterpret_cast< ImVec4* >( cfg.esp_aimbot_color ) );
                c_ui_elements::end_group( );
            } );

                ImGui::EndChild( );
                ImGui::PopStyleColor( );
                ImGui::PopStyleVar( );

                ImGui::TableNextColumn( );
                c_ui_elements::begin_group( "PREVIEW", { GetContentRegionAvail( ).x, 420.f }, true );
                {
                ImGuiWindow* w = GetCurrentWindow( );

                ImVec2 const pa = ImGui::GetContentRegionAvail( );
                ImVec2 const box_sz( ImMin( 150.f, pa.x - 80.f ), ImMin( 300.f, pa.y - 40.f ) );
                ImVec2 const box_min( w->Pos.x + ( w->Size.x - box_sz.x ) * 0.5f,
                                      w->Pos.y + ( w->Size.y - box_sz.y ) * 0.5f );
                ImRect const bounds( box_min, box_min + box_sz );

                static c_esp_layout preview;
                static bool preview_init = false;
                if ( !preview_init )
                {
                    preview_init = true;

                    auto text_measure = []( const char* text ) {
                        return [ text ]( ImRect const&, esp_side, float, void* ) -> ImVec2 {
                            return ImGui::CalcTextSize( text );
                        };
                    };
                    auto text_render = []( const char* text, float const* col ) {
                        return [ text, col ]( ImDrawList* dl, ImRect const&, esp_side, ImVec2 p,
                                              float a, float vis, void* ) {
                            dl->AddText( p, IM_COL32( (int)( col[0] * 255 ), (int)( col[1] * 255 ),
                                                      (int)( col[2] * 255 ), (int)( 255 * a * vis ) ), text );
                        };
                    };

                    preview.add_box( 1, "Box", []( ImDrawList* dl, ImRect const& b, esp_side, ImVec2,
                                                   float a, float vis, void* ) {
                        dl->AddRect( b.Min, b.Max,
                            IM_COL32( (int)( cfg.esp_box_color[0] * 255 ), (int)( cfg.esp_box_color[1] * 255 ),
                                      (int)( cfg.esp_box_color[2] * 255 ), (int)( 210 * a * vis ) ), 0.f, 0, 1.5f );
                    } );

                    preview.add_text( 2, "Name", esp_side::top, 0, text_measure( "Player" ),
                                      text_render( "Player", cfg.esp_name_color ) );

                    preview.add_bar( 3, "Health", esp_side::left, 0,
                        []( ImRect const& b, esp_side side, float, void* ) -> ImVec2 {
                            bool const horizontal = side == esp_side::top || side == esp_side::bottom;
                            return horizontal ? ImVec2( b.GetWidth( ), 4.f )
                                              : ImVec2( 4.f, b.GetHeight( ) );
                        },
                        []( ImDrawList* dl, ImRect const& b, esp_side side, ImVec2 p, float a, float vis, void* ) {
                            float const frac = 0.72f;
                            ImU32 const hi = IM_COL32( (int)( cfg.esp_health_max[0] * 255 ),
                                (int)( cfg.esp_health_max[1] * 255 ), (int)( cfg.esp_health_max[2] * 255 ),
                                (int)( 255 * a * vis ) );
                            ImU32 const bg = IM_COL32( 0, 0, 0, (int)( 120 * a * vis ) );
                            if ( side == esp_side::top || side == esp_side::bottom ) {
                                float const w = b.GetWidth( );
                                dl->AddRectFilled( { p.x, p.y }, { p.x + w, p.y + 4.f }, bg );
                                if ( cfg.esp_health_gradient ) {
                                    ImU32 const lo = IM_COL32( (int)( cfg.esp_health_min[0] * 255 ),
                                        (int)( cfg.esp_health_min[1] * 255 ), (int)( cfg.esp_health_min[2] * 255 ),
                                        (int)( 255 * a * vis ) );
                                    dl->AddRectFilledMultiColor( { p.x, p.y }, { p.x + w * frac, p.y + 4.f },
                                                                 lo, hi, hi, lo );
                                } else {
                                    dl->AddRectFilled( { p.x, p.y }, { p.x + w * frac, p.y + 4.f }, hi );
                                }
                            } else {
                                float const h = b.GetHeight( );
                                dl->AddRectFilled( { p.x, p.y }, { p.x + 4.f, p.y + h }, bg );
                                if ( cfg.esp_health_gradient ) {
                                    ImU32 const lo = IM_COL32( (int)( cfg.esp_health_min[0] * 255 ),
                                        (int)( cfg.esp_health_min[1] * 255 ), (int)( cfg.esp_health_min[2] * 255 ),
                                        (int)( 255 * a * vis ) );
                                    dl->AddRectFilledMultiColor( { p.x, p.y + h * ( 1.f - frac ) }, { p.x + 4.f, p.y + h },
                                                                 hi, hi, lo, lo );
                                } else {
                                    dl->AddRectFilled( { p.x, p.y + h * ( 1.f - frac ) }, { p.x + 4.f, p.y + h }, hi );
                                }
                            }
                        } );

                    preview.add_text( 4, "Weapon", esp_side::bottom, 0, text_measure( "AK-47" ),
                                      text_render( "AK-47", cfg.esp_weapon_color ) );
                    preview.add_text( 5, "Ammo", esp_side::right, 5, text_measure( "30 / 90" ),
                                      text_render( "30 / 90", cfg.esp_ammo_max ) );

                    // icons (undefeated glyphs, same set the reference UI uses)
                    auto icon_measure = []( const char* glyph ) {
                        return [ glyph ]( ImRect const&, esp_side, float, void* ) -> ImVec2 {
                            auto& f = alice::core->fonts->get( "undefeated" );
                            if ( !f.ptr ) return { 0.f, 0.f };
                            return f.ptr->CalcTextSizeA( f.size, FLT_MAX, -1, glyph );
                        };
                    };
                    auto icon_render = []( const char* glyph, float const* col ) {
                        return [ glyph, col ]( ImDrawList* dl, ImRect const&, esp_side, ImVec2 p,
                                               float a, float vis, void* ) {
                            auto& f = alice::core->fonts->get( "undefeated" );
                            if ( !f.ptr ) return;
                            dl->AddText( f.ptr, f.size, p, IM_COL32( (int)( col[0] * 255 ), (int)( col[1] * 255 ),
                                (int)( col[2] * 255 ), (int)( 255 * a * vis ) ), glyph );
                        };
                    };

                    preview.add_text( 11, "Weapon icon", esp_side::bottom, 2, icon_measure( "W" ),
                                      icon_render( "W", cfg.esp_weapon_icon_color ) );
                    preview.add_text( 12, "Kit icon", esp_side::left, 2, icon_measure( "r" ),
                                      icon_render( "r", cfg.esp_kit_icon_color ) );
                    preview.add_text( 13, "C4 icon", esp_side::left, 3, icon_measure( "o" ),
                                      icon_render( "o", cfg.esp_c4_icon_color ) );
                    preview.add_text( 14, "Taser icon", esp_side::left, 1, icon_measure( "h" ),
                                      icon_render( "h", cfg.esp_taser_icon_color ) );

                    // separate flags down the right side
                    preview.add_text( 6, "Money", esp_side::right, 0, text_measure( "$800" ),
                                      text_render( "$800", cfg.flag_money_color ) );
                    preview.add_text( 7, "Armor flag", esp_side::right, 1, text_measure( "HK" ),
                                      text_render( "HK", cfg.flag_armor_color ) );
                    preview.add_text( 8, "Kit flag", esp_side::right, 2, text_measure( "KIT" ),
                                      text_render( "KIT", cfg.flag_kit_color ) );
                    preview.add_text( 9, "Defuse flag", esp_side::right, 3, text_measure( "DEFUSE" ),
                                      text_render( "DEFUSE", cfg.flag_defuse_color ) );
                    preview.add_text( 10, "Flash flag", esp_side::right, 4, text_measure( "FLASH" ),
                                      text_render( "FLASH", cfg.flag_flashed_color ) );
                    preview.add_text( 16, "C4 flag", esp_side::right, 6, text_measure( "C4" ),
                                      text_render( "C4", cfg.flag_c4_color ) );

                    preview.add_bar( 15, "Armor", esp_side::bottom, 3,
                        []( ImRect const& b, esp_side, float, void* ) -> ImVec2 { return { b.GetWidth( ), 4.f }; },
                        []( ImDrawList* dl, ImRect const& b, esp_side, ImVec2 p, float a, float vis, void* ) {
                            ImVec2 const a0( p.x, p.y );
                            ImVec2 const a1( p.x + b.GetWidth( ), p.y + 4.f );
                            dl->AddRectFilled( a0, a1, IM_COL32( 0, 0, 0, (int)( 120 * a * vis ) ) );
                            ImVec2 const fill( p.x + b.GetWidth( ) * 0.7f, p.y + 4.f );
                            if ( cfg.esp_armor_gradient ) {
                                ImU32 const lo = IM_COL32( (int)( cfg.esp_armor_min[0] * 255 ),
                                    (int)( cfg.esp_armor_min[1] * 255 ), (int)( cfg.esp_armor_min[2] * 255 ),
                                    (int)( 255 * a * vis ) );
                                ImU32 const hi = IM_COL32( (int)( cfg.esp_armor_max[0] * 255 ),
                                    (int)( cfg.esp_armor_max[1] * 255 ), (int)( cfg.esp_armor_max[2] * 255 ),
                                    (int)( 255 * a * vis ) );
                                dl->AddRectFilledMultiColor( a0, fill, lo, hi, hi, lo );
                            } else {
                                dl->AddRectFilled( a0, fill, IM_COL32( (int)( cfg.esp_armor_max[0] * 255 ),
                                    (int)( cfg.esp_armor_max[1] * 255 ), (int)( cfg.esp_armor_max[2] * 255 ),
                                    (int)( 255 * a * vis ) ) );
                            }
                            dl->AddRect( { a0.x - 1, a0.y - 1 }, { a1.x + 1, a1.y + 1 },
                                         IM_COL32( 0, 0, 0, (int)( 160 * a * vis ) ) );
                        } );

                }

                preview.set_enabled( 1, cfg.esp_box );
                preview.set_enabled( 2, cfg.esp_name );
                preview.set_enabled( 3, cfg.esp_health );
                preview.set_enabled( 4, cfg.esp_weapon );
                preview.set_enabled( 5, cfg.esp_ammo );
                preview.set_enabled( 6, cfg.esp_flags && cfg.flag_money );
                preview.set_enabled( 7, cfg.esp_flags && cfg.flag_armor );
                preview.set_enabled( 8, cfg.esp_flags && cfg.flag_kit );
                preview.set_enabled( 9, cfg.esp_flags && cfg.flag_defuse );
                preview.set_enabled( 10, cfg.esp_flags && cfg.flag_flashed );
                preview.set_enabled( 16, cfg.esp_flags && cfg.flag_c4 );
                preview.set_enabled( 11, cfg.esp_weapon_icon );
                preview.set_enabled( 12, cfg.esp_kit_icon );
                preview.set_enabled( 13, cfg.esp_c4_icon );
                preview.set_enabled( 14, cfg.esp_taser_icon );
                preview.set_enabled( 15, cfg.esp_armor );

                bool const interactive = ImGui::IsWindowHovered( );
                preview.render( w->DrawList, bounds, 1.f, nullptr, interactive );

                }
                c_ui_elements::end_group( );
                ImGui::EndTable( );
            }
        }
    };

    class visuals_move_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Movement"; }
        std::string icon( ) const override { return ICON_LUCIDE_MOVE; }

        void render( ) override
        {
            c_ui_elements::begin_group( "MOVEMENT", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Bunnyhop", &cfg.bhop, &cfg.kb_bhop, &cfg.kb_mode_bhop );
            bind_row( "Auto strafe", &cfg.autostrafe, &cfg.kb_autostrafe, &cfg.kb_mode_autostrafe );
            {
                int mode = cfg.strafe_follow_keys ? 1 : 0;
                c_ui_elements::start( "Strafe direction" ).add_dropdown( &mode, lists::strafe_modes( ) ).finish( );
                cfg.strafe_follow_keys = ( mode == 1 );
            }
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "EXTRAS", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row_ex( "Infinite duck", &cfg.inf_duck, FB_INF_DUCK );
            bind_row_ex( "Crouch in air", &cfg.crouch_air, FB_CROUCH_AIR );
            bind_row_ex( "Fake duck", &cfg.fake_duck, FB_FAKE_DUCK );
            bind_row_ex( "Quick stop", &cfg.quick_stop, FB_QUICK_STOP );
            c_ui_elements::start( "Fast walk" ).add_slider( &cfg.fastwalk, 1.0f, 2.0f, "%.2fx" ).finish( );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "EXPLOITS", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Duck peek", &cfg.x_duckpeek, &cfg.kb_duckpeek, &cfg.kb_mode_duckpeek );
            c_ui_elements::start( "Rapid crouch toggle while held" ).finish( );
            bind_row( "Silent run", &cfg.x_silentrun, &cfg.kb_silentrun, &cfg.kb_mode_silentrun );
            c_ui_elements::start( "Crouch taps reset footstep timer" ).finish( );
            bind_row( "Jump bug", &cfg.x_jumpbug, &cfg.kb_jumpbug, &cfg.kb_mode_jumpbug );
            c_ui_elements::start( "Crouch before landing: no fall damage" ).finish( );
            bind_row( "Fast ladder", &cfg.x_fastladder, &cfg.kb_fastladder, &cfg.kb_mode_fastladder );
            c_ui_elements::start( "Forward + strafe while climbing (~2x)" ).finish( );
            bind_row( "Reload cancel (Q)", &cfg.x_qcancel, &cfg.kb_qcancel, &cfg.kb_mode_qcancel,
                      nullptr, nullptr, "lastinv twice on press" );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "CAMERA", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row_ex( "Third person", &cfg.thirdperson, FB_THIRDPERSON );
            c_ui_elements::start( "Distance" ).add_slider( &cfg.thirdperson_dist, 30.0f, 500.0f, "%.0f" ).finish( );
            c_ui_elements::end_group( );
        }
    };

    class visuals_fx_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Effects"; }
        std::string icon( ) const override { return ICON_LUCIDE_SPARKLES; }

        void render( ) override
        {
            c_ui_elements::begin_group( "BULLET IMPACTS", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Bullet Impacts" ).add_toggle( &cfg.bullet_impacts ).finish( );
            c_ui_elements::visible_if( "impact_cols", cfg.bullet_impacts, [ & ] {
                c_ui_elements::start( "Client color (red)" ).add_color( reinterpret_cast< ImVec4* >( cfg.impact_color ) ).finish( );
                c_ui_elements::start( "Server color (blue)" ).add_color( reinterpret_cast< ImVec4* >( cfg.impact_server_color ) ).finish( );
                c_ui_elements::start( "Marker size" ).add_slider( &cfg.impact_size, 1.0f, 12.0f, "%.1f" ).finish( );
                c_ui_elements::start( "Marker life (s)" ).add_slider( &cfg.impact_time, 0.2f, 8.0f, "%.1f" ).finish( );
            } );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "TRACERS", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Draw tracers" ).add_toggle( &cfg.tracers ).finish( );
            c_ui_elements::visible_if( "tracer_opts", cfg.tracers, [ & ] {
                c_ui_elements::start( "Local color" ).add_color( reinterpret_cast< ImVec4* >( cfg.tracer_local_color ) ).finish( );
                c_ui_elements::start( "Enemy color" ).add_color( reinterpret_cast< ImVec4* >( cfg.tracer_enemy_color ) ).finish( );
                c_ui_elements::start( "Teammate color" ).add_color( reinterpret_cast< ImVec4* >( cfg.tracer_team_color ) ).finish( );
                c_ui_elements::start( "Tracer life (s)" ).add_slider( &cfg.tracer_life, 0.5f, 6.0f, "%.1f" ).finish( );
            } );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "KILL EFFECTS", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Kill effect" ).add_toggle( &cfg.kill_effects ).finish( );
            c_ui_elements::visible_if( "killfx_opts", cfg.kill_effects, [ & ] {
                c_ui_elements::start( "Effect" ).add_dropdown( &cfg.kill_effect_type, lists::kill_effects( ) ).finish( );
                c_ui_elements::start( "FX color" ).add_color( reinterpret_cast< ImVec4* >( cfg.kill_effect_color ) ).finish( );
                c_ui_elements::start( "FX scale" ).add_slider( &cfg.kill_effect_scale, 0.2f, 4.0f, "%.1f" ).finish( );
                c_ui_elements::start( "FX life (s)" ).add_slider( &cfg.kill_effect_time, 0.2f, 6.0f, "%.1f" ).finish( );
            } );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "GRENADES", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Grenade prediction" ).add_toggle( &cfg.nade_predict ).finish( );
            c_ui_elements::start( "Grenade markers" ).add_toggle( &cfg.nade_esp ).finish( );
            c_ui_elements::visible_if( "nade_opts", cfg.nade_predict || cfg.nade_esp, [ & ] {
                c_ui_elements::start( "HE color" ).add_color( reinterpret_cast< ImVec4* >( cfg.nade_color_he ) ).finish( );
                c_ui_elements::start( "Flash color" ).add_color( reinterpret_cast< ImVec4* >( cfg.nade_color_flash ) ).finish( );
                c_ui_elements::start( "Smoke color" ).add_color( reinterpret_cast< ImVec4* >( cfg.nade_color_smoke ) ).finish( );
            } );
            c_ui_elements::end_group( );
        }
    };

    class visuals_page : public c_ui_page
    {
    public:
        visuals_page( )
        {
            register_subpage< visuals_esp_sub >( );
            register_subpage< visuals_move_sub >( );
            register_subpage< visuals_fx_sub >( );
        }
        std::string name( ) const override { return "Visuals"; }
        std::string icon( ) const override { return ICON_LUCIDE_EYE; }
    };

    // ------------------------------------------------------------------
    // World / Settings
    // ------------------------------------------------------------------

    class models_chams_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Chams"; }
        std::string icon( ) const override { return ICON_LUCIDE_LAYERS; }

        static void layer_group( const char* title, Config::ChamsLayer& layer )
        {
            c_ui_elements::begin_group( title, { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Enabled" ).add_toggle( &layer.enabled )
                .add_color( reinterpret_cast< ImVec4* >( layer.color ) ).finish( );
            c_ui_elements::visible_if( std::string( title ) + "_opts", layer.enabled, [ & ] {
                c_ui_elements::start( "Material" ).add_dropdown( &layer.material, lists::chams_types( ) ).finish( );
            } );
            c_ui_elements::end_group( );
        }

        void render( ) override
        {
            static int target_idx = 0;
            Config::ChamsTarget* targets[ 5 ] = { &cfg.chams_enemy, &cfg.chams_team, &cfg.chams_local, &cfg.chams_viewmodel, &cfg.chams_weapons };

            c_ui_elements::begin_group( "TARGET", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Chams target" ).add_dropdown( &target_idx, lists::chams_targets( ) ).finish( );
            c_ui_elements::end_group( );

            target_idx = ImClamp( target_idx, 0, 4 );
            Config::ChamsTarget* target = targets[ target_idx ];

            layer_group( "THROUGH WALLS", target->occluded );
            layer_group( "VISIBLE", target->visible );
            layer_group( "OVERLAY", target->overlay );

            c_ui_elements::begin_group( "FAKE MODEL CHAMS", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Fake model chams" ).add_toggle( &cfg.chams_fake ).finish( );
            c_ui_elements::visible_if( "fake_chams_opts", cfg.chams_fake, [ & ] {
                c_ui_elements::start( "Color" ).add_color( reinterpret_cast< ImVec4* >( cfg.chams_fake_color ) ).finish( );
            } );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "HIT CHAMS", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row( "Enabled", &cfg.chams_hit, &cfg.kb_hitchams, &cfg.kb_mode_hitchams );
            c_ui_elements::visible_if( "hit_chams_opts", cfg.chams_hit, [ & ] {
                c_ui_elements::start( "Material" ).add_dropdown( &cfg.chams_hit_material, lists::chams_types( ) ).finish( );
                c_ui_elements::start( "Color" ).add_color( reinterpret_cast< ImVec4* >( cfg.chams_hit_color ) ).finish( );
                c_ui_elements::start( "Duration" ).add_slider( &cfg.chams_hit_duration, 0.1f, 5.0f, "%.1fs" ).finish( );
            } );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "GLOBAL", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row_ex( "Disable occlusion", &cfg.chams_disable_occlusion, FB_CHAMS_DISABLE_OCC );
            c_ui_elements::end_group( );
        }
    };

    class models_custom_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Custom"; }
        std::string icon( ) const override { return ICON_LUCIDE_BOX; }

        void render( ) override
        {
            c_ui_elements::begin_group( "PLAYER MODEL", { GetContentRegionAvail( ).x, 99999.f } );
            {
                int const count = CustomModels_Count( false );
                std::vector< std::string > entries;
                entries.reserve( count + 1 );
                entries.push_back( "Default" );
                int model_sel = 0;
                for ( int i = 0; i < count; i++ ) {
                    const char* p = CustomModels_At( i );
                    entries.push_back( p );
                    if ( cfg.model_path[0] && _stricmp( cfg.model_path, p ) == 0 )
                        model_sel = i + 1;
                }

                int prev = model_sel;
                c_ui_elements::start( "Model" ).add_dropdown( &model_sel, entries ).finish( );
                if ( model_sel != prev )
                {
                    if ( model_sel <= 0 ) {
                        cfg.model_path[0] = 0;
                        cfg.model_index = 0;
                    } else {
                        strncpy( cfg.model_path, entries[ model_sel ].c_str( ), sizeof( cfg.model_path ) - 1 );
                        cfg.model_path[ sizeof( cfg.model_path ) - 1 ] = 0;
                        cfg.model_index = 0;
                    }
                }
                c_ui_elements::start( CustomModels_Status( ) ).finish( );
                c_ui_elements::start( "Put .mdl packs in cstrike/models/player" ).finish( );
            }
            c_ui_elements::end_group( );
        }
    };

    class models_page : public c_ui_page
    {
    public:
        models_page( )
        {
            register_subpage< models_chams_sub >( );
            register_subpage< models_custom_sub >( );
        }
        std::string name( ) const override { return "Models"; }
        std::string icon( ) const override { return ICON_LUCIDE_LAYERS; }
    };

    class world_look_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "World"; }
        std::string icon( ) const override { return ICON_LUCIDE_GLOBE; }

        void render( ) override
        {
            c_ui_elements::begin_group( "WORLD", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row_ex( "Fullbright", &cfg.world_fullbright, FB_FULLBRIGHT );
            bind_row_ex( "No fog", &cfg.world_nofog, FB_NOFOG );
            c_ui_elements::visible_if( "world_fog", !cfg.world_nofog, [ & ] {
                bind_row_ex( "Custom fog", &cfg.world_custom_fog, FB_CUSTOMFOG );
                c_ui_elements::visible_if( "world_fog_opts", cfg.world_custom_fog, [ & ] {
                    c_ui_elements::start( "Fog start" ).add_slider( &cfg.fog_start, 0.0f, 5000.0f, "%.0f" ).finish( );
                    c_ui_elements::start( "Fog end" ).add_slider( &cfg.fog_end, 0.0f, 8000.0f, "%.0f" ).finish( );
                    c_ui_elements::start( "Fog density" ).add_slider( &cfg.fog_density, 0.0f, 100.0f, "%.0f" ).finish( );
                    c_ui_elements::start( "Fog color" ).add_color( reinterpret_cast< ImVec4* >( cfg.fog_color ) ).finish( );
                } );
            } );
            bind_row_ex( "No 3D skybox", &cfg.world_nosky, FB_NOSKY );
            c_ui_elements::start( "Brightness" ).add_slider( &cfg.world_brightness, 0.0f, 2.0f, "%.2f" ).finish( );
            c_ui_elements::start( "Night level" ).add_slider( &cfg.world_night, 0.0f, 1.0f, "%.2f" ).finish( );
            c_ui_elements::start( "Model ambient" ).add_slider( &cfg.world_model_ambient, 0.0f, 1.0f, "%.2f" ).finish( );
            c_ui_elements::start( "Bloom" ).add_slider( &cfg.world_bloom, 0.0f, 4.0f, "%.2f" ).finish( );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "WORLD COLOR", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Tint props" ).add_toggle( &cfg.world_props ).finish( );
            c_ui_elements::visible_if( "world_prop_col", cfg.world_props, [ & ] {
                c_ui_elements::start( "Prop color" ).add_color( reinterpret_cast< ImVec4* >( cfg.prop_color ) ).finish( );
            } );
            c_ui_elements::start( "Tint walls" ).add_toggle( &cfg.world_walls ).finish( );
            c_ui_elements::visible_if( "world_wall_col", cfg.world_walls, [ & ] {
                c_ui_elements::start( "Wall color" ).add_color( reinterpret_cast< ImVec4* >( cfg.wall_color ) ).finish( );
            } );
            c_ui_elements::end_group( );
        }
    };

    class world_view_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "View"; }
        std::string icon( ) const override { return ICON_LUCIDE_CAMERA; }

        void render( ) override
        {
            c_ui_elements::begin_group( "VIEW", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "FOV override (0 = default)" ).add_slider( &cfg.fov_override, 0.0f, 160.0f, "%.0f" ).finish( );
            c_ui_elements::start( "No scope zoom" ).add_toggle( &cfg.no_scope_zoom ).finish( );
            c_ui_elements::start( "Override viewmodel FOV" ).add_slider( &cfg.viewmodel_fov, 0.0f, 160.0f, "%.0f" ).finish( );
            c_ui_elements::start( "Viewmodel offset X" ).add_slider( &cfg.viewmodel_x, -20.0f, 20.0f, "%.1f" ).finish( );
            c_ui_elements::start( "Viewmodel offset Y" ).add_slider( &cfg.viewmodel_y, -20.0f, 20.0f, "%.1f" ).finish( );
            c_ui_elements::start( "Viewmodel offset Z" ).add_slider( &cfg.viewmodel_z, -20.0f, 20.0f, "%.1f" ).finish( );
            bind_row_ex( "Left hand", &cfg.left_hand, FB_LEFT_HAND );
            bind_row_ex( "Remove viewmodel bob", &cfg.remove_bob, FB_REMOVE_BOB );
            c_ui_elements::end_group( );
        }
    };

    class world_remove_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Removals"; }
        std::string icon( ) const override { return ICON_LUCIDE_EYE_OFF; }

        void render( ) override
        {
            c_ui_elements::begin_group( "REMOVALS", { GetContentRegionAvail( ).x, 99999.f } );
            bind_row_ex( "Post processing", &cfg.rem_postprocess, FB_REM_POST );
            bind_row_ex( "Shadows", &cfg.rem_shadows, FB_REM_SHADOWS );
            bind_row_ex( "Decals", &cfg.rem_decals, FB_REM_DECALS );
            bind_row_ex( "Grass / detail props", &cfg.rem_grass, FB_REM_GRASS );
            bind_row_ex( "Ropes", &cfg.rem_ropes, FB_REM_ROPES );
            bind_row_ex( "Water reflections", &cfg.rem_water, FB_REM_WATER );
            bind_row_ex( "Dynamic lights", &cfg.rem_dynlights, FB_REM_DYN );
            bind_row_ex( "Shell casings", &cfg.rem_shells, FB_REM_SHELLS );
            bind_row_ex( "Debris / ragdolls", &cfg.rem_debris, FB_REM_DEBRIS );
            bind_row_ex( "Particles / smoke", &cfg.rem_particles, FB_REM_PARTICLES );
            c_ui_elements::end_group( );
        }
    };

    class world_overlay_sub : public c_ui_subpage
    {
    public:
        std::string name( ) const override { return "Overlays"; }
        std::string icon( ) const override { return ICON_LUCIDE_LAYERS; }

        void render( ) override
        {
            c_ui_elements::begin_group( "OVERLAYS", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Watermark" ).add_toggle( &cfg.watermark ).finish( );
            c_ui_elements::start( "Aim FOV circle" ).add_toggle( &cfg.draw_aim_fov ).finish( );
            c_ui_elements::start( "Penetration crosshair" ).add_toggle( &cfg.pen_crosshair ).finish( );
            c_ui_elements::visible_if( "pen_xhair_opts", cfg.pen_crosshair, [ & ] {
                static std::vector< std::string > pen_styles = { "3D Box", "2D Surface Box" };
                c_ui_elements::start( "Box style" ).add_dropdown( &cfg.pen_crosshair_style, pen_styles ).finish( );
                c_ui_elements::start( "Penetration dot" ).add_toggle( &cfg.pen_crosshair_dot ).finish( );
            } );
            c_ui_elements::start( "Cheat revealer (TAB)" ).add_toggle( &cfg.cheat_revealer ).finish( );
            static std::vector< std::string > spoof_list = { "Off", "Islamware", "Epoximotion", "Poptape", "Claret", "Killed", "Neverlose", "Skeet", "Insomnia" };
            c_ui_elements::start( "Cheat spoofer" ).add_dropdown( &cfg.cheat_spoofer, spoof_list ).finish( );
            c_ui_elements::end_group( );
        }
    };

    class world_page : public c_ui_page
    {
    public:
        world_page( )
        {
            register_subpage< world_look_sub >( );
            register_subpage< world_view_sub >( );
            register_subpage< world_remove_sub >( );
            register_subpage< world_overlay_sub >( );
        }
        std::string name( ) const override { return "World"; }
        std::string icon( ) const override { return ICON_LUCIDE_GLOBE; }
    };

    // ------------------------------------------------------------------
    // config page helpers (alicekit primitives only)
    // ------------------------------------------------------------------
    static void cfg_text( const char* text, ImVec4 const& color, float pad_y = 6.f )
    {
        ImVec2 const p = GetCursorScreenPos( ) + ImVec2( 15.f, pad_y );
        ImU32 const col = ColorConvertFloat4ToU32(
            ImVec4( color.x, color.y, color.z, color.w * GetStyle( ).Alpha ) );
        GetWindowDrawList( )->AddText( p, col, text );
        Dummy( ImVec2( 0.f, pad_y + GetFontSize( ) + pad_y ) );
    }

    static bool cfg_input_full( ImVec2 const& pos, float width, char* buf, int size, const char* id )
    {
        auto const& t = c_themes::palette( );
        float const h = 30.f;

        SetCursorScreenPos( pos );

        PushStyleColor( ImGuiCol_FrameBg, ImVec4( 1.f / 255.f, 1.f / 255.f, 1.f / 255.f, 1.f ) );
        PushStyleColor( ImGuiCol_FrameBgHovered, ImVec4( 1.f / 255.f, 1.f / 255.f, 1.f / 255.f, 1.f ) );
        PushStyleColor( ImGuiCol_FrameBgActive, ImVec4( 1.f / 255.f, 1.f / 255.f, 1.f / 255.f, 1.f ) );
        PushStyleColor( ImGuiCol_Border, ImVec4( 3.f / 255.f, 3.f / 255.f, 3.f / 255.f, 1.f ) );
        PushStyleColor( ImGuiCol_Text, t.text );
        PushStyleColor( ImGuiCol_TextSelectedBg, t.accent_bg );
        PushStyleVar( ImGuiStyleVar_FrameRounding, 6.f );
        PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.f );
        PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 10.f, ( h - GetFontSize( ) ) * 0.5f ) );
        PushItemWidth( width );
        SetNextItemWidth( width );

        bool const changed = InputText( id, buf, size );

        PopItemWidth( );
        PopStyleVar( 3 );
        PopStyleColor( 6 );
        return changed;
    }

    static bool cfg_button( const char* label, ImVec2 const& size )
    {
        bool hit = false;
        c_ui_elements::select_button( label, &hit, size );
        return hit;
    }

    class settings_page : public c_ui_page
    {
    public:
        std::string name( ) const override { return "Settings"; }
        std::string icon( ) const override { return ICON_LUCIDE_SETTINGS; }

        void render( ) override
        {
            c_ui_elements::begin_group( "KEYBINDS", { GetContentRegionAvail( ).x, 99999.f } );
            bind_input_row( "Menu open key", &cfg.kb_menu );
            c_ui_elements::end_group( );

            c_ui_elements::begin_group( "MENU", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Custom menu (ImGui)" ).add_toggle( &cfg.menu_imgui ).finish( );
            c_ui_elements::start( "Click a bind box, then press a key" ).finish( );
            c_ui_elements::start( "Right-click a function to bind it" ).finish( );
#ifdef _WIN64
            c_ui_elements::start( "Interface: VEngineClient014 / x64" ).finish( );
#else
            c_ui_elements::start( "Interface: VEngineClient012 / v34 (x86)" ).finish( );
#endif
            c_ui_elements::end_group( );

            // server browser fix: Valve retired the master server this build's
            // emu queries, so the Internet tab is empty.  The toggle refreshes a
            // community list into the emu's serverbrowser.txt at startup.
            // The connect fix disables SteamNetworkingSockets so the engine
            // falls back to legacy UDP (fixes "Connection failed after 4 retries").
            c_ui_elements::begin_group( "SERVER BROWSER", { GetContentRegionAvail( ).x, 99999.f } );
            c_ui_elements::start( "Fix community server list" ).add_toggle( &cfg.browser_fix ).finish( );
            {
                char status[ 128 ];
                ServerListStatus( status, sizeof( status ) );
                c_ui_elements::start( status ).finish( );
            }

            // emulator LAN-only mode: without this key gbe_fork hooks
            // connect/sendto and silently drops every non-LAN packet
            {
                bool prev = cfg.allow_internet;
                c_ui_elements::start( "Allow internet (gamemode: LAN-only off)" ).add_toggle( &cfg.allow_internet ).finish( );
                if ( cfg.allow_internet != prev )
                    ApplyEmuNetworkFix( cfg.allow_internet );
            }
            c_ui_elements::end_group( );

            // Refresh lives outside the card: the browser-fix box closes above,
            // the button sits on the page background, then the connectivity box
            // opens again below.
            ImGui::Dummy( ImVec2( 0.f, 10.f ) );
            if ( cfg_button( "Refresh now", { GetContentRegionAvail( ).x - 15.f, 30.f } ) )
                StartServerListRefresh( "menu" );
            ImGui::Dummy( ImVec2( 0.f, 10.f ) );

            c_ui_elements::begin_group( "CONNECTIVITY", { GetContentRegionAvail( ).x, 99999.f } );
            {
                bool prev = cfg.connect_fix;
                c_ui_elements::start( "Fix direct connect (off fixes retries)" ).add_toggle( &cfg.connect_fix ).finish( );
                if ( cfg.connect_fix != prev )
                    SetConnectFix( cfg.connect_fix );
            }
            {
                char status[ 128 ];
                ConnectFixStatus( status, sizeof( status ) );
                c_ui_elements::start( status ).finish( );
            }

            // current servers reject builds older than their own; the engine
            // reports this string from cstrike/steam.inf in the connect packet
            {
                bool prev = cfg.spoof_version;
                c_ui_elements::start( "Report current build to servers" ).add_toggle( &cfg.spoof_version ).finish( );
                if ( cfg.spoof_version != prev )
                    SetVersionSpoof( cfg.spoof_version );
            }
            {
                // raw ImGui widgets do not get the row inset the kit's items
                // use, so both the label and the field are shifted by it
                ImGui::SetCursorPosX( ImGui::GetCursorPosX( ) + 15.f );
                ImGui::TextUnformatted( "Reported build (restart to apply)" );
                ImGui::SetCursorPosX( ImGui::GetCursorPosX( ) + 15.f );
                ImGui::SetNextItemWidth( GetContentRegionAvail( ).x - 15.f );
                ImGui::InputText( "##reported_build", cfg.reported_build, sizeof( cfg.reported_build ) );
            }
            c_ui_elements::end_group( );


            c_ui_elements::begin_group( "STATE", { GetContentRegionAvail( ).x, 99999.f } );
            {
                char buf[ 96 ];
                int local = engine.GetLocalPlayer( );
                snprintf( buf, sizeof( buf ), "Local player index %d", local );
                c_ui_elements::start( buf ).finish( );

                void* local_ent = IsGameReady( ) && local > 0 ? entity_list.GetClientEntity( local ) : nullptr;
                if ( local_ent )
                {
                    snprintf( buf, sizeof( buf ), "Health %d / Team %d",
                              *reinterpret_cast< int* >( ( char* ) local_ent + off::m_iHealth ),
                              *reinterpret_cast< int* >( ( char* ) local_ent + off::m_iTeamNum ) );
                    c_ui_elements::start( buf ).finish( );
                }
            }
            c_ui_elements::end_group( );

            {
                ImGuiWindow* w = GetCurrentWindow( );
                char const* note = "alicekit imgui";
                ImVec2 const ts = ImGui::CalcTextSize( note );
                ImVec2 const p( w->Pos.x + w->Size.x - ts.x - 12.0f,
                                w->Pos.y + w->Size.y - ts.y - 10.0f );
                w->DrawList->AddText( p, IM_COL32( 110, 110, 115, 200 ), note );
            }
        }
    };

    class config_page : public c_ui_page
    {
    public:
        std::string name( ) const override { return "Config"; }
        std::string icon( ) const override { return ICON_LUCIDE_SAVE; }

        void render( ) override
        {
            config_init( );
            auto const& t = c_themes::palette( );

            float const avail = GetContentRegionAvail( ).x;
            float const col_h = ImMax( 150.f, GetContentRegionAvail( ).y - 24.f );
            float const gap = 10.f;
            float const row_gap = 8.f;
            float const btn_h = 30.f;
            float const list_w = ImMax( 230.f, avail * 0.46f );
            // Leave one button row + gap below the list box.
            float const list_h = ImMax( 120.f, col_h - btn_h - row_gap );

            ImVec2 const columns_origin = GetCursorScreenPos( );

            c_ui_elements::begin_group( "CONFIGS", { list_w, list_h }, true );
            {
                Dummy( ImVec2( 0.f, 10.f ) );

                if ( g_configs.empty( ) )
                {
                    cfg_text( "No saved configs", t.text_muted );
                }
                else
                {
                    for ( int i = 0; i < ( int ) g_configs.size( ); i++ )
                    {
                        bool const was_selected = ( g_config_sel == i );
                        bool selected = was_selected;

                        SetCursorPosX( 15.f );
                        c_ui_elements::select_button( g_configs[ i ], &selected,
                            { GetContentRegionAvail( ).x - 15.f, 30.f } );

                        if ( selected && !was_selected )
                        {
                            g_config_sel = i;
                            strncpy( g_config_name, g_configs[ i ].c_str( ), sizeof( g_config_name ) - 1 );
                            g_config_name[ sizeof( g_config_name ) - 1 ] = 0;
                        }
                    }
                }
            }
            c_ui_elements::end_group( );

            // Two list actions side by side, outside and below the list box,
            // spanning the same width as the box itself.
            float const left_x = columns_origin.x;
            float const left_w = list_w;
            float const left_y = columns_origin.y + list_h + row_gap;

            SetCursorScreenPos( { left_x, left_y } );
            if ( cfg_button( "Delete", { left_w, btn_h } ) )
            {
                config_delete( g_config_name );
                g_config_sel = -1;
            }

            // Settings column on the right.
            SetCursorScreenPos( { columns_origin.x + list_w + gap, columns_origin.y } );

            c_ui_elements::begin_group( "CONFIG", { avail - list_w - gap, col_h }, true );
            {
                ImVec2 const r0 = GetCursorScreenPos( );
                // Equal 15px inset on both sides so the field and buttons are
                // centred inside the box instead of touching the right edge.
                float const inner_w = GetContentRegionAvail( ).x;
                float const cw = inner_w - 30.f;
                float const cx = r0.x + ( inner_w - cw ) * 0.5f;

                float const top = r0.y + 10.f;

                cfg_input_full( { cx, top }, cw, g_config_name, sizeof( g_config_name ), "##cfg_name" );

                float const row_y = top + btn_h + 10.f;
                float const bw = ( cw - row_gap ) * 0.5f;

                SetCursorScreenPos( { cx, row_y } );
                if ( cfg_button( "Save", { bw, btn_h } ) )
                {
                    config_save( g_config_name );
                    g_config_sel = -1;
                }

                SetCursorScreenPos( { cx + bw + row_gap, row_y } );
                if ( cfg_button( "Load", { bw, btn_h } ) )
                    config_load( g_config_name );

                SetCursorScreenPos( { r0.x, row_y + btn_h + 8.f } );
                if ( g_config_sel >= 0 && g_config_sel < ( int ) g_configs.size( ) )
                {
                    std::string const line = "Selected: " + g_configs[ g_config_sel ];
                    cfg_text( line.c_str( ), t.text_light );
                }
                else
                {
                    cfg_text( "No config selected", t.text_muted );
                }

                cfg_text( "Saved next to css.exe in islamware_configs", t.text_hint, 2.f );
            }
            c_ui_elements::end_group( );
        }
    };
} // namespace

namespace alice_ui
{
    void Init( IDirect3DDevice9* device, int width, int height )
    {
        if ( g_initialized )
            return;

        core = std::make_unique< c_core >( );

        core->assets->init_loader( []( void const* data, size_t size, ImTextureID* out, bool ) {
            *out = load_png( data, size );
        } );

        core->init_backend< c_dx9_backend >( device, width, height );
        if ( !core->backend || !core->blur )
            return;

        core->assets->load_premade( );
        core->fonts->initialize( );
        core->fonts->bind( );
        core->ui->initialize( );
        core->blur->init_duplication( );

        core->ui->register_page< rage_page >( );
        core->ui->register_page< visuals_page >( );
        core->ui->register_page< models_page >( );
        core->ui->register_page< world_page >( );
        core->ui->register_page< config_page >( );
        core->ui->register_page< settings_page >( );

        {
            char note[ 64 ];
            if ( cfg.kb_menu )
                snprintf( note, sizeof( note ), "Press %s to open / close", vk_name( cfg.kb_menu ) );
            else
                snprintf( note, sizeof( note ), "No menu key bound" );
            c_notifications::push( notification_type::tip, "Menu", note, 8.f );
        }

        g_initialized = true;
        g_open = false;
        cfg.menu_open = false;
        core->ui->set_presence( false );
    }

    void Shutdown( )
    {
        if ( core )
        {
            if ( core->fonts && core->fonts->get_loader( ) )
                core->fonts->get_loader( )->release( );
            if ( core->assets )
                core->assets->release( );
            if ( core->blur )
                core->blur->resize( 0, 0 );
        }
        core.reset( );
        g_initialized = false;
        g_open = false;
    }

    void OnLostDevice( )
    {
        if ( core )
        {
            if ( core->blur )
                core->blur->resize( 0, 0 );
            if ( core->fonts && core->fonts->get_loader( ) )
                core->fonts->get_loader( )->release_gpu( );
            if ( core->assets )
                core->assets->release( );
        }
    }

    void OnResetDevice( IDirect3DDevice9* device, int width, int height )
    {
        ( void ) device;
        if ( core && core->blur )
            core->blur->resize( width, height );

        // Default-pool textures (font atlas, logo/banner) are destroyed on
        // Reset.  Re-upload the atlas without rebuilding the ImFont objects,
        // which would leave dangling pointers in the UI state.
        if ( core && core->fonts )
            core->fonts->rebind( );
        if ( core && core->assets )
            core->assets->load_premade( );
    }

    void PrepareFrame( )
    {
        if ( !g_initialized || !g_open || !core || !core->blur )
            return;
        core->blur->prepare_frame( );
    }

    void Render( )
    {
        if ( !g_initialized || !core || !core->ui )
            return;

        // Never leave the bind window alive while the menu is closed: it is a
        // top-level window and would stay on screen, sitting on top of the
        // menu when it is reopened.
        if ( !cfg.menu_open )
        {
            g_bind_window_open = false;
            g_bind_capturing = false;
            g_bind_capture = false;
            g_bind_request = false;
            g_bind.target = nullptr;
            g_bind.mode = nullptr;
        }

        core->ui->render( );
        draw_keybind_popup( );
    }

    void Toggle( )
    {
        g_open = !cfg.menu_open;
        cfg.menu_open = g_open;
        if ( g_initialized && core && core->ui )
            core->ui->set_presence( g_open );
    }

    void SetOpen( bool open )
    {
        g_open = open;
        cfg.menu_open = open;
        if ( g_initialized && core && core->ui )
            core->ui->set_presence( open );
        if ( !open )
        {
            g_bind_window_open = false;
            g_bind_capturing = false;
            g_bind_capture = false;
            g_bind.target = nullptr;
            g_bind.mode = nullptr;
        }
    }

    bool IsOpen( )
    {
        return g_open;
    }

    bool IsBinding( )
    {
        return g_bind_capture;
    }

    // The config statics live in the anonymous namespace above; these
    // wrappers expose them to the alt menu.
    void ConfigMenuInit( )
    {
        config_init( );
    }

    int ConfigMenuCount( )
    {
        config_init( );
        return static_cast< int >( g_configs.size( ) );
    }

    const char* ConfigMenuName( int index )
    {
        config_init( );
        return ( index >= 0 && index < static_cast< int >( g_configs.size( ) ) ) ? g_configs[ index ].c_str( ) : nullptr;
    }

    void ConfigMenuSave( const char* name )
    {
        config_save( name ? name : "" );
    }

    void ConfigMenuLoad( const char* name )
    {
        config_load( name ? name : "" );
    }

    void ConfigMenuDelete( const char* name )
    {
        config_delete( name ? name : "" );
    }

    void ConfigMenuRefresh( )
    {
        config_refresh( );
    }
} // namespace alice_ui
