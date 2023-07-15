/*** include ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** define ***/

#define ROAMR_VERSION "0.0.1"

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
 * @brief get the sursor position. `n` command can be used to query the terminal
 * for status information. argument of 6 to ask for the cursor position.
 *
 * @param rows row of the cursor
 * @param cols column of the cursor
 * @return success or not.
 */
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      break;
    }

    if (buf[i] == 'R') {
      break;
    }

    i++;
  }

  buf[i] = '\0';

  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
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
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) {
      return -1;
    }

    return getCursorPosition(rows, cols);
  } else {
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** append buffer ***/

typedef struct abuf_t {
  char *b;
  int len;
} abuf;

#define ABUF_INIT                                                              \
  { NULL, 0 }

/**
 * @brief Append a string s to an abuf.
 *
 * @param ab append buf
 * @param s string
 * @param len length
 */
void abAppend(abuf *ab, const char *s, int len) {
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) {
    return;
  }

  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

/**
 * @brief deallocates the dynamic memory used by abuf.
 *
 * @param ab append buf
 */
void abFree(abuf *ab) { free(ab->b); }

/*** output ***/

/**
 * @brief Draw a column of tildes(~) on the left of the screen, like vim.
 *
 * @param ab write() the append buf contens out to standard
 */
void editorDrawRows(abuf *ab) {
  for (int i = 0; i < E.screenrows; i++) {
    if (i == E.screenrows / 3) {
      char welcome[80];
      int welcomelen = snprintf(welcome, sizeof(welcome),
                                "Roamr editor -- version %s", ROAMR_VERSION);
      if (welcomelen > E.screencols) {
        welcomelen = E.screencols;
      }

      // center
      int padding = (E.screencols - welcomelen) / 2;
      if (padding) {
        abAppend(ab, "~", 1);
        padding--;
      }
      while (padding--) {
        abAppend(ab, " ", 1);
      }

      abAppend(ab, welcome, welcomelen);
    } else {
      abAppend(ab, "~", 1);
    }

    abAppend(ab, "\x1b[K", 3);
    if (i < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

/**
 * @brief Clear the screen
 */
void editorRefreshScreen() {
  abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l]h", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);

  abAppend(&ab, "\x1b[H", 3);
  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
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
