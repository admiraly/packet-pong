#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "net.h"
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "font.h"
#include "post.h"

enum { W=480, H=270, TOP=34, BOTTOM=245, PAD=35, BALL=5, STEP=8 };
enum { MENU, ADDRESS, HOST, JOIN };
static struct { int x,y,vx,vy,pad[2],score[2],wait; } game;
static unsigned char pixels[W*H];
static int mode, effect, direction=1;
static char address[80], status[80];
static GLuint textures[2], program;
static int frame;
static PFNGLUSEPROGRAMPROC use_program;
static PFNGLUNIFORM1IPROC uniform_int;
static PFNGLUNIFORM1FPROC uniform_float;
static PFNGLACTIVETEXTUREPROC active_texture;
static GLint effect_loc, time_loc;

static int clamp(int value,int low,int high) {
    return value<low ? low : value>high ? high : value;
}

static void serve(void) {
    game.x=W*50; game.y=(TOP+BOTTOM)*50;
    game.vx=direction*160; game.vy=(SDL_GetTicks()&1)?85:-85;
    game.wait=650/STEP;
}

static void reset_game(void) {
    memset(&game,0,sizeof game);
    game.pad[0]=game.pad[1]=(TOP+BOTTOM-PAD)/2;
    serve();
}

static void tick(int left,int right) {
    game.pad[0]=clamp(game.pad[0]+left*2,TOP,BOTTOM-PAD);
    game.pad[1]=clamp(game.pad[1]+right*2,TOP,BOTTOM-PAD);
    if (game.wait) { --game.wait; return; }
    int old_x=game.x;
    game.x+=game.vx; game.y+=game.vy;
    if (game.y<TOP*100 || game.y>(BOTTOM-BALL)*100) {
        game.y=clamp(game.y,TOP*100,(BOTTOM-BALL)*100);
        game.vy=-game.vy;
    }
    int side=game.vx>0;
    int edge=(side ? W-17-BALL : 17)*100;
    int crossing=side ? old_x<=edge && game.x>=edge : old_x>=edge && game.x<=edge;
    if (crossing && game.y+BALL*100>game.pad[side]*100 && game.y<(game.pad[side]+PAD)*100) {
        game.x=edge;
        game.vx=(side?-1:1)*clamp((side?game.vx:-game.vx)+16,160,420);
        game.vy=(game.y/100+BALL/2-game.pad[side]-PAD/2)*12;
    }
    if (game.x < -BALL*100 || game.x > W*100) {
        int winner=game.x>0?0:1;
        game.score[winner]=(game.score[winner]+1)%100;
        direction=winner?-1:1;
        serve();
    }
}

static void rect(int x,int y,int width,int height) {
    int right=clamp(x+width,0,W), bottom=clamp(y+height,0,H);
    x=clamp(x,0,W);
    for (int row=clamp(y,0,H); row<bottom && x<right; ++row)
        memset(pixels+row*W+x,255,(size_t)(right-x));
}

static void center(int y,const char *text,int scale) {
    int x=(W-(int)strlen(text)*6*scale+scale)/2;
    for (; *text; ++text,x+=6*scale) {
        int ch=toupper((unsigned char)*text);
        int index=ch>='A'&&ch<='Z'?ch-'A':ch>='0'&&ch<='9'?ch-'0'+26:
            ch=='-'?36:ch=='/'?37:ch=='.'?38:ch==':'?39:ch=='%'?40:ch=='_'?41:-1;
        if (index<0) continue;
        for (int col=0; col<5; ++col)
            for (int row=0; row<7; ++row)
                if (alphabet[index][col]&(1<<row)) rect(x+col*scale,y+row*scale,scale,scale);
    }
}

static void draw(void) {
    char text[80];
    memset(pixels,0,sizeof pixels);
    if (net.link) {
        snprintf(text,sizeof text,"%02d   %02d",game.score[0],game.score[1]);
        center(5,text,3);
        snprintf(text,sizeof text,"LINK / %u MS",net.rtt);
        center(28,text,1);
        for (int y=42; y<BOTTOM; y+=10) rect(W/2-1,y,2,5);
        rect(12,game.pad[0],5,PAD); rect(W-17,game.pad[1],5,PAD);
        rect(game.x/100,game.y/100,BALL,BALL);
        center(254,"W/S OR ARROWS / F1-F4 FX / ESC LEAVE",1);
    } else if (mode==ADDRESS) {
        center(99,"HOST IP",2);
        center(127,*address?address:"_",1);
        center(154,"ENTER JOIN / ESC BACK",1);
    } else if (mode==MENU) {
        center(112,"H HOST / J JOIN",2);
        center(141,status,1);
    } else {
        center(113,mode==HOST?"WAITING FOR PLAYER":"CONNECTING",2);
        center(143,mode==HOST?"UDP 42874":address,1);
        center(254,"ESC BACK",1);
    }
}

static int setup_gl(void) {
    PFNGLCREATESHADERPROC create_shader=(PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
    PFNGLSHADERSOURCEPROC shader_source=(PFNGLSHADERSOURCEPROC)SDL_GL_GetProcAddress("glShaderSource");
    PFNGLCOMPILESHADERPROC compile_shader=(PFNGLCOMPILESHADERPROC)SDL_GL_GetProcAddress("glCompileShader");
    PFNGLGETSHADERIVPROC shader_info=(PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
    PFNGLGETSHADERINFOLOGPROC shader_log=(PFNGLGETSHADERINFOLOGPROC)SDL_GL_GetProcAddress("glGetShaderInfoLog");
    PFNGLCREATEPROGRAMPROC create_program=(PFNGLCREATEPROGRAMPROC)SDL_GL_GetProcAddress("glCreateProgram");
    PFNGLATTACHSHADERPROC attach=(PFNGLATTACHSHADERPROC)SDL_GL_GetProcAddress("glAttachShader");
    PFNGLLINKPROGRAMPROC link=(PFNGLLINKPROGRAMPROC)SDL_GL_GetProcAddress("glLinkProgram");
    PFNGLGETPROGRAMIVPROC program_info=(PFNGLGETPROGRAMIVPROC)SDL_GL_GetProcAddress("glGetProgramiv");
    PFNGLGETPROGRAMINFOLOGPROC program_log=(PFNGLGETPROGRAMINFOLOGPROC)SDL_GL_GetProcAddress("glGetProgramInfoLog");
    PFNGLGETUNIFORMLOCATIONPROC location=(PFNGLGETUNIFORMLOCATIONPROC)SDL_GL_GetProcAddress("glGetUniformLocation");
    use_program=(PFNGLUSEPROGRAMPROC)SDL_GL_GetProcAddress("glUseProgram");
    uniform_int=(PFNGLUNIFORM1IPROC)SDL_GL_GetProcAddress("glUniform1i");
    uniform_float=(PFNGLUNIFORM1FPROC)SDL_GL_GetProcAddress("glUniform1f");
    active_texture=(PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
    if (!create_shader || !shader_source || !compile_shader || !shader_info || !shader_log ||
        !create_program || !attach || !link || !program_info || !program_log || !location ||
        !use_program || !uniform_int || !uniform_float || !active_texture) return 0;

    GLint ok;
    char log[1024];
    GLuint shader=create_shader(GL_FRAGMENT_SHADER);
    shader_source(shader,1,&post_source,NULL);
    compile_shader(shader);
    shader_info(shader,GL_COMPILE_STATUS,&ok);
    if (!ok) { shader_log(shader,sizeof log,NULL,log); fprintf(stderr,"%s\n",log); return 0; }
    program=create_program();
    attach(program,shader); link(program);
    program_info(program,GL_LINK_STATUS,&ok);
    if (!ok) { program_log(program,sizeof log,NULL,log); fprintf(stderr,"%s\n",log); return 0; }
    use_program(program);
    uniform_int(location(program,"scene"),0);
    uniform_int(location(program,"previous"),1);
    effect_loc=location(program,"effect"); time_loc=location(program,"seconds");
    glGenTextures(2,textures);
    for (int index=0; index<2; ++index) {
        glBindTexture(GL_TEXTURE_2D,textures[index]);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D,0,GL_LUMINANCE8,W,H,0,GL_LUMINANCE,GL_UNSIGNED_BYTE,pixels);
    }
    return 1;
}

static void present(SDL_Window *window,uint32_t now) {
    int width,height;
    SDL_GL_GetDrawableSize(window,&width,&height);
    int view_w=width,view_h=width*H/W;
    if (view_h>height) { view_h=height; view_w=height*W/H; }
    glViewport((width-view_w)/2,(height-view_h)/2,view_w,view_h);
    glClear(GL_COLOR_BUFFER_BIT);
    active_texture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,textures[frame^1]);
    active_texture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,textures[frame]);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,W,H,GL_LUMINANCE,GL_UNSIGNED_BYTE,pixels);
    uniform_int(effect_loc,effect); uniform_float(time_loc,now/1000.0f);
    glBegin(GL_QUADS);
    glTexCoord2f(0,1); glVertex2f(-1,-1);
    glTexCoord2f(1,1); glVertex2f(1,-1);
    glTexCoord2f(1,0); glVertex2f(1,1);
    glTexCoord2f(0,0); glVertex2f(-1,1);
    glEnd();
    SDL_GL_SwapWindow(window);
    frame^=1;
}

static int start(int next) {
    SDL_StopTextInput();
    if (!net_open(next==HOST?NULL:address,SDL_GetPerformanceCounter(),SDL_GetTicks())) {
        snprintf(status,sizeof status,"%s",next==HOST?"CANNOT BIND UDP 42874":"INVALID IP OR SOCKET ERROR");
        mode=MENU;
        return 0;
    }
    status[0]=0;
    mode=next;
    reset_game();
    return 1;
}

int main(int argc,char **argv) {
    int initial=MENU, smoke=0;
    if (argc==2 && !strcmp(argv[1],"--host")) initial=HOST;
    else if (argc==3 && !strcmp(argv[1],"--join") && strlen(argv[2])<sizeof address) {
        initial=JOIN; strcpy(address,argv[2]);
    } else if (argc==2 && !strcmp(argv[1],"--smoke-test")) smoke=1;
    else if (argc!=1) { fprintf(stderr,"Usage: %s [--host | --join IP | --smoke-test]\n",argv[0]); return 2; }
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO)!=0) { fprintf(stderr,"SDL: %s\n",SDL_GetError()); return 1; }
    if (!net_init()) { fprintf(stderr,"Socket initialization failed\n"); SDL_Quit(); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_Window *window=SDL_CreateWindow("Packet Pong",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
        W*2,H*2,SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI|
        (smoke?SDL_WINDOW_HIDDEN:0));
    SDL_GLContext context=window?SDL_GL_CreateContext(window):NULL;
    int result=1;
    if (!context || !setup_gl()) { fprintf(stderr,"OpenGL 2.1 initialization failed: %s\n",SDL_GetError()); goto done; }
    SDL_GL_SetSwapInterval(1);
    SDL_StopTextInput();
    if (initial!=MENU && !start(initial)) { fprintf(stderr,"%s\n",status); goto done; }
    reset_game();
    uint32_t last=SDL_GetTicks(), accumulator=0;
    int running=1, frames=0, linked=0;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type==SDL_QUIT) running=0;
            if (event.type==SDL_TEXTINPUT && mode==ADDRESS) {
                size_t length=strlen(address);
                for (const char *ch=event.text.text; *ch && length<sizeof address-1; ++ch)
                    if (isalnum((unsigned char)*ch) || strchr(":.%_-",*ch)) address[length++]=*ch;
                address[length]=0;
            }
            if (event.type!=SDL_KEYDOWN || event.key.repeat) continue;
            SDL_Keycode key=event.key.keysym.sym;
            if (key>=SDLK_F1 && key<=SDLK_F4) {
                int selected=key-SDLK_F1+1;
                effect=effect==selected?0:selected;
            } else if (key==SDLK_ESCAPE && mode!=MENU) {
                net_close(); SDL_StopTextInput(); mode=MENU; linked=0;
            } else if (mode==MENU && key==SDLK_h) start(HOST);
            else if (mode==MENU && key==SDLK_j) { mode=ADDRESS; address[0]=0; SDL_StartTextInput(); }
            else if (mode==ADDRESS && key==SDLK_BACKSPACE && *address) address[strlen(address)-1]=0;
            else if (mode==ADDRESS && key==SDLK_RETURN && *address) start(JOIN);
        }
        const Uint8 *keys=SDL_GetKeyboardState(NULL);
        int move=(keys[SDL_SCANCODE_S]||keys[SDL_SCANCODE_DOWN])-
                 (keys[SDL_SCANCODE_W]||keys[SDL_SCANCODE_UP]);
        uint32_t now=SDL_GetTicks();
        if (mode==HOST || mode==JOIN) {
            net_receive(now);
            if (mode==HOST && net.link && !linked) reset_game();
            linked=net.link;
            accumulator+=(uint32_t)clamp((int)(now-last),0,64);
            while (accumulator>=STEP) {
                if (net.link && mode==HOST) tick(move,net.move);
                accumulator-=STEP;
            }
            if (mode==JOIN && net.link) {
                game.x=net.state[0]; game.y=net.state[1];
                game.pad[0]=net.state[2]; game.pad[1]=net.state[3];
                game.score[0]=net.state[4]; game.score[1]=net.state[5];
            }
            int snapshot[]={game.x,game.y,game.pad[0],game.pad[1],game.score[0],game.score[1]};
            net_send(now,move,snapshot);
        }
        last=now;
        if (smoke) effect=frames;
        draw(); present(window,now);
        if (smoke && ++frames==5) running=0;
        SDL_Delay(1);
    }
    result=0;
done:
    net_close(); net_shutdown();
    if (context) SDL_GL_DeleteContext(context);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
