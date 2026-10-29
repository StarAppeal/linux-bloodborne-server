#define _GNU_SOURCE
#include <assert.h>
#include <unistd.h>
#include "../src/runtime_pad.c"

static int capture;
int bbgpu_overlay_captures_input(void) { return capture; }
void bbgpu_game_thread_tick(void) {}
/* Co-op autoplay (gpu/bbcoop): what the rig's script holds now. */
static int autoplay;
static uint32_t autoplay_buttons;
static uint8_t autoplay_sticks[4]={128,128,128,128};
int bbcoop_autoplay_pad(uint32_t *buttons, uint8_t sticks[4]) {
    if (!autoplay) return 0;
    *buttons=autoplay_buttons;
    memcpy(sticks,autoplay_sticks,4);
    return 1;
}
/* Party death (gpu/bbcoop): a downed player keeps only the menu and the camera. */
static int downed;
void bbcoop_party_death_pad(uint32_t *buttons, uint8_t sticks[4], uint8_t *l2, uint8_t *r2) {
    if (!downed) return;
    *buttons&=BTN_OPTIONS;
    sticks[0]=sticks[1]=128;
    *l2=*r2=0;
}
/* Kaniel 2026-10-25: the free camera flies: the pad is its own, the game gets nothing. */
static int flying;
void bbcoop_free_camera_pad(uint32_t *buttons, uint8_t sticks[4], uint8_t *l2, uint8_t *r2) {
    if (!flying) return;
    *buttons=0;
    memset(sticks,128,4);
    *l2=*r2=0;
}
int bbcoop_camera_mouse(float dx, float dy) { (void)dx; (void)dy; return 0; }
/* A cutscene under the team's loading screen: Options pressed for its skip. */
static int skip;
int bbcoop_cutscene_skip(void) { return skip; }
/* A party's cutscene: Options is a vote (0 the player's, 1 held back, 2 pressed for the skip). */
static int vote, vote_options=-1;
int bbcoop_cutscene_vote_pad(int options) { vote_options=options; return vote; }
/* The team menu in game just closed: Circle for the game's page under it. */
static int menu_back;
int bbcoop_team_menu_back(void) { return menu_back; }
/* The touchpad held: the map opens. */
static int map_opened;
void bbcoop_map_from_pad(void) { ++map_opened; }
/* A double tap on the touchpad: a team ping (Kaniel 2026-10-24). */
static int pings_asked;
void bbcoop_ping_aim(void) { ++pings_asked; }
/* The mouse is tested apart (test_mouse_camera.c); here the window has none. */
void bbgpu_mouse_enable(int enabled) { (void)enabled; }
void bbgpu_mouse_set_direct(void (*turn)(float, float), void (*drop)(void)) { (void)turn; (void)drop; }
int bbgpu_mouse_take(float *dx, float *dy, float *wheel, uint32_t *buttons) { *dx=*dy=*wheel=0; *buttons=0; return 0; }
int runtime_camhook_install(int no_auto_rotation, const char **why) { (void)no_auto_rotation; *why="test"; return 0; }
int runtime_camhook_active(void) { return 0; }
void runtime_camhook_turn(float pitch, float yaw) { (void)pitch; (void)yaw; }
void runtime_camhook_drop(void) {}
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *name) {
    (void)table; (void)count; (void)name;
    return 0;
}

static void inject(const char *path, const char *tokens) {
    FILE *f=fopen(path,"w");
    assert(f);
    fputs(tokens,f);
    fclose(f);
    usleep(25000);
}

int main(void) {
    char path[]="/tmp/bbport-pad-test-XXXXXX";
    int fd=mkstemp(path);
    assert(fd>=0);
    close(fd);
    setenv("BB_PAD_FILE",path,1);
    /* bbport.ini controls: buttons moved, a trigger as a button and a button as a trigger. */
    char config[]="/tmp/bbport-pad-config-XXXXXX";
    int config_fd=mkstemp(config);
    assert(config_fd>=0);
    const char controls[]="upscaler=fsr3\npad.cross=b\npad.circle=a\npad.r2=rightshoulder\n"
                          "pad.r1=righttrigger\nkey.cross=X, Space\npad.bogus=a\n";
    assert(write(config_fd,controls,sizeof(controls)-1)==(ssize_t)(sizeof(controls)-1));
    close(config_fd);
    setenv("BB_CONFIG",config,1);
    setenv("SDL_VIDEODRIVER","dummy",1);
    /* Only the virtual test controller is a gamepad, whatever is plugged in. */
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0x1d50/0x6189");
    assert(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_GAMEPAD));
    assert(pad_init()==0 && pad_open(1,0,0,NULL)==1);
    PadData data;
    inject(path,"cross l3 touchpad_left");
    assert(pad_read_state(1,&data)==0);
    assert((data.buttons & (BTN_CROSS|BTN_L3|BTN_TOUCHPAD))==(BTN_CROSS|BTN_L3|BTN_TOUCHPAD));
    /* A new touch gets a new id (1..127), as from a DualShock 4: the game ignores id 0. */
    assert(data.touch_count==1 && data.touches[0].x==480 && data.touches[0].y==471 && data.touches[0].id==1);
    inject(path,"touchpad_right");
    assert(pad_read_state(1,&data)==0 && data.touch_count==1 && data.touches[0].x==1440);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);
    /* Autoplay: its buttons on top of the pad's, and the sticks it moves; not under the menu. */
    autoplay=1; autoplay_buttons=BTN_CIRCLE; autoplay_sticks[0]=200;
    inject(path,"cross");
    assert(pad_read_state(1,&data)==0 && data.buttons==(BTN_CROSS|BTN_CIRCLE) && data.connected);
    assert(data.left_x==200 && data.left_y==128 && data.right_x==128 && data.right_y==128);
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.left_x==128);
    /* Kaniel 2026-10-15: the loading screen takes the pad, but a cutscene under it skips. */
    skip=1;
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_OPTIONS);
    skip=0;
    capture=0;
    autoplay=0; autoplay_sticks[0]=128;
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    /* Kaniel 2026-10-16: the team screen over the game closed: Circle closes the game's page too. */
    menu_back=1;
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_CIRCLE);
    menu_back=0;
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    /* Kaniel 2026-10-17: under a party's cutscene Options is a vote: held back until everyone
     * watching voted, then pressed for the game's skip. */
    vote=1;
    inject(path,"options cross");
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_CROSS && vote_options==1);
    vote=2;
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_OPTIONS && vote_options==0);
    vote=0;
    inject(path,"options");
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_OPTIONS);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    /* Party death: what the game reads, autoplay's buttons and sticks too, goes through it. */
    downed=1; autoplay=1; autoplay_buttons=BTN_R1; autoplay_sticks[0]=200; autoplay_sticks[2]=60;
    inject(path,"cross options r2");
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_OPTIONS && data.r2==0);
    assert(data.left_x==128 && data.left_y==128 && data.right_x==60 && data.right_y==128);
    downed=0; autoplay=0; autoplay_sticks[0]=128; autoplay_sticks[2]=128;
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);
    /* The free camera after party death: while it flies, nothing reaches the game. */
    flying=1;
    inject(path,"cross r2");
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.r2==0 && data.left_x==128);
    flying=0;
    inject(path,"cross");
    assert(pad_read_state(1,&data)==0 && data.buttons==BTN_CROSS);
    inject(path,"");
    assert(pad_read_state(1,&data)==0 && data.buttons==0);

    SDL_VirtualJoystickTouchpadDesc touch={.nfingers=2};
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type=SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes=SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons=SDL_GAMEPAD_BUTTON_COUNT;
    desc.button_mask=(1u<<SDL_GAMEPAD_BUTTON_COUNT)-1;
    desc.axis_mask=(1u<<SDL_GAMEPAD_AXIS_COUNT)-1;
    desc.name="bbport test controller";
    desc.vendor_id=0x1d50;
    desc.product_id=0x6189;
    desc.ntouchpads=1;
    desc.touchpads=&touch;
    SDL_JoystickID id=SDL_AttachVirtualJoystick(&desc);
    assert(id!=0);
    SDL_Joystick *joystick=SDL_OpenJoystick(id);
    assert(joystick);
    /* The SDL touchpad (Kaniel 2026-10-18/24, runtime_pad.c map_hold_step): while it is held the
     * game sees neither its button nor its fingers; a hold of MAP_HOLD_US opens the port's map; a
     * lone short tap reaches the game TOUCH_DOUBLE_US after it is let go, as a click on the side it
     * was pressed; two taps within that time are a team ping, and the game sees neither. */
    const int map_before=map_opened, pings_before=pings_asked;
    /* Two fingers, the right one first, and the touchpad pressed: held, nothing reaches the game. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,true,0.25f,1.0f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(gamepad && data.touch_count==0 && !(data.buttons & BTN_TOUCHPAD));
    capture=1;
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && data.buttons==0);
    capture=0;
    /* Held past MAP_HOLD_US: the map opens once, and the game still sees no touchpad. */
    usleep((MAP_HOLD_US+20000u));
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && !(data.buttons & BTN_TOUCHPAD));
    assert(map_opened==map_before+1);
    assert(pad_read_state(1,&data)==0 && map_opened==map_before+1);
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,1,false,0,0,0));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    /* Let go after the map opened: no click for the game, then or later. */
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);
    usleep(TOUCH_DOUBLE_US+20000u);
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    /* A lone short tap on the right half: nothing while it may still become a double tap, then
     * the game's click on that side. */
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.75f,0.5f,1.0f));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && data.touch_count==0 && !(data.buttons & BTN_TOUCHPAD));
    assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_TOUCHPAD));
    usleep(TOUCH_DOUBLE_US+10000u); /* within the click's MAP_TAP_US after that */
    assert(pad_read_state(1,&data)==0 && (data.buttons & BTN_TOUCHPAD));
    assert(data.touch_count==1 && data.touches[0].x==1440 && data.touches[0].y==471);
    assert(map_opened==map_before+1 && pings_asked==pings_before);
    usleep(MAP_TAP_US+20000u);
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);

    /* Two taps within TOUCH_DOUBLE_US: one team ping, and no click for the game. */
    for (int tap=0;tap<2;++tap) {
        assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,true,0.25f,0.5f,1.0f));
        assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,true));
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_TOUCHPAD));
        assert(SDL_SetJoystickVirtualTouchpad(joystick,0,0,false,0,0,0));
        assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_TOUCHPAD,false));
        SDL_UpdateJoysticks();
        SDL_UpdateGamepads();
        assert(pad_read_state(1,&data)==0 && !(data.buttons & BTN_TOUCHPAD));
    }
    assert(pings_asked==pings_before+1);
    usleep(TOUCH_DOUBLE_US+MAP_TAP_US+20000u);
    assert(pad_read_state(1,&data)==0 && data.buttons==0 && data.touch_count==0);
    assert(map_opened==map_before+1 && pings_asked==pings_before+1);
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_EAST,true));
    assert(SDL_SetJoystickVirtualButton(joystick,SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,true));
    assert(SDL_SetJoystickVirtualAxis(joystick,SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,32767));
    SDL_UpdateJoysticks();
    SDL_UpdateGamepads();
    assert(pad_read_state(1,&data)==0);
    assert(data.buttons==(BTN_CROSS|BTN_R2|BTN_R1) && data.r2==255);
    assert(bindings[IN_CROSS].key_count==2 && bindings[IN_CROSS].keys[0].code==SDL_SCANCODE_X &&
           bindings[IN_CROSS].keys[1].code==SDL_SCANCODE_SPACE);
    SDL_CloseJoystick(joystick);
    if (gamepad) SDL_CloseGamepad(gamepad);
    gamepad=NULL;
    assert(SDL_DetachVirtualJoystick(id));
    SDL_Quit();
    unlink(path);
    unlink(config);
    puts("PASS: pad ABI, debug camera chord, left/right clicks, SDL touchpad hold for the map, tap and double-tap ping, overlay capture, controls, co-op autoplay, party death");
}
