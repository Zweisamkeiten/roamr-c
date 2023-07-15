/*** include ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** define ***/

#define CTRL_KEY(k) ((k)&0x1f)

/*** data ***/

typedef struct editorConfig_t {
  int screenrows;
  int screencols;
  struct termios orig_termios;
} editor_Config;

editor_Config E;

/*** terminal ***/

/**
 * @brief Error handling
 *
 * @param s error message
 */
void die(const char *s) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  // perror looks at the global `errno` and print *s before error message.
  perror(s);
  exit(EXIT_FAILURE);
}

/**
 * @brief Disable raw mode at exit
 */
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
    die("tcsetattr");
}

/**
 * @brief Turn off echoing
 */
void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
    die("tcsetattr");
  atexit(disableRawMode);

  struct termios raw = E.orig_termios;

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/**
 * @brief wait for one keypress and return it.
 *
 * @return return the keypress.
 */
char editorReadKey() {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    // when read timeout return -1 & errno EAGAIN
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  return c;
}

/**
 * @brief get terminal WindowSize
 *
 * @param rows editor E struct
 * @param cols editor E struct
 * @return success or not.
 */
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    return -1;
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** output ***/

/**
 * @brief Draw a column of tildes(~) on the left of the screen, like vim.
 */
void editorDrawRows() {
  for (int i = 0; i < E.screenrows; i++) {
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}

/**
 * @brief Clear the screen
 */
void editorRefreshScreen() {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  editorDrawRows();

  write(STDERR_FILENO, "\x1b[H", 3);
}

/*** input ***/

void editorProcessKeypress() {
  char c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'): {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }
  }
}

/*** init ***/

/**
 * @brief initialize all the fields in the E struct
 */
void initEditor() {
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return EXIT_SUCCESS;
}
