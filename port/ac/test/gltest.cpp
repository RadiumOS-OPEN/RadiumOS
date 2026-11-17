// Host test for softgl.cpp: clang++ -O2 -I include softgl.cpp test/gltest.cpp -o /tmp/gltest && /tmp/gltest out.ppm
#include "GL/gl.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char **argv)
{
    const int W = 640, H = 360;
    unsigned *fb = (unsigned *)calloc(W * H, 4);
    softgl_attach(fb, W, H);

    unsigned tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    unsigned char img[64 * 64 * 4];
    for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
        int c = ((x / 8 + y / 8) & 1) ? 230 : 40;
        img[(y * 64 + x) * 4 + 0] = c; img[(y * 64 + x) * 4 + 1] = x * 4; img[(y * 64 + x) * 4 + 2] = y * 4; img[(y * 64 + x) * 4 + 3] = 255;
    }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);

    glViewport(0, 0, W, H);
    glClearColor(0.2f, 0.3f, 0.5f, 1); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glFrustum(-0.8, 0.8, -0.45, 0.45, 1, 200);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    glRotatef(20, 1, 0, 0); glTranslatef(0, -2, -3);
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_FOG); glFogi(GL_FOG_MODE, GL_LINEAR); glFogf(GL_FOG_START, 5); glFogf(GL_FOG_END, 60);
    float fc[4] = {0.2f, 0.3f, 0.5f, 1}; glFogfv(GL_FOG_COLOR, fc);

    glBegin(GL_QUADS);                    // a floor
    glColor3f(1, 1, 1);
    glTexCoord2f(0, 0); glVertex3f(-20, 0, 10);
    glTexCoord2f(0, 40); glVertex3f(-20, 0, -80);
    glTexCoord2f(40, 40); glVertex3f(20, 0, -80);
    glTexCoord2f(40, 0); glVertex3f(20, 0, 10);
    glEnd();

    glDisable(GL_FOG);
    glBegin(GL_TRIANGLES);                // a lit wall with vertex colours
    glColor3f(1, 0.2f, 0.2f); glTexCoord2f(0, 0); glVertex3f(-2, 0, -8);
    glColor3f(0.2f, 1, 0.2f); glTexCoord2f(2, 0); glVertex3f(2, 0, -8);
    glColor3f(0.2f, 0.2f, 1); glTexCoord2f(1, 2); glVertex3f(0, 3, -8);
    glEnd();

    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDisable(GL_TEXTURE_2D);
    glBegin(GL_QUADS);
    glColor4f(1, 1, 0, 0.5f); glVertex3f(-1, 0.5f, -6); glVertex3f(1, 0.5f, -6); glVertex3f(1, 2, -6); glVertex3f(-1, 2, -6);
    glEnd();

    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, W, H, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity(); glDisable(GL_DEPTH_TEST);
    glColor4f(1, 1, 1, 1); glLineWidth(3);
    glBegin(GL_LINES); glVertex2f(10, 10); glVertex2f(200, 60); glEnd();

    FILE *f = fopen(argc > 1 ? argv[1] : "out.ppm", "wb");
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) { fputc(fb[i] >> 16, f); fputc(fb[i] >> 8, f); fputc(fb[i], f); }
    fclose(f);
    return 0;
}
