/*** include ***/

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** define ***/

#define ROAMR_VERSION "0.0.1"
#define ROAMR_TAB_STOP 8
#define ROAMR_QUIT_TIMES 3

#define CTRL_KEY(k) ((k)&0x1f)

enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*** data ***/

typedef struct erow_t {
  int size;
  int rsize;
  char *chars;
  char *render;
} erow;

typedef struct editorConfig_t {
  int cx, cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct termios orig_termios;
} editor_Config;

editor_Config E;

/*** prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt);

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
int editorReadKey() {
  int nread;
  char c;

  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    // when read timeout return -1 & errno EAGAIN
    if (nread == -1 && errno != EAGAIN)
      die("read");
  }

  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1)
      return '\x1b';
    if (read(STDIN_FILENO, &seq[1], 1) != 1)
      return '\x1b';

    if (seq[0] == '[') {

      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1)
          return '\x1b';

        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }

      } else {

        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        case 'P':
          return DEL_KEY;
        }
      }
    } else if (seq[0] == '0') {

      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    return '\x1b';
  } else {
    return c;
  }
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

/*** row opearations ***/

/**
 * @brief loop through all the characters to the left of cx, and figure out how
 many spaces each tab takes up.
 *
 * @param row current row
 * @param cx char x
 * @return render x
 */
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  for (int j = 0; j < cx; j++) {
    if (row->chars[j] == '\t') {
      rx += (ROAMR_TAB_STOP - 1) - (rx % ROAMR_TAB_STOP);
    }
    rx++;
  }
  return rx;
}

/**
 * @brief render tabs as multiple space characters
 *
 * @param row row
 */
void editorUpdateRow(erow *row) {
  int tabs = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      tabs++;
    }
  }

  free(row->render);
  row->render = malloc(row->size + tabs * (ROAMR_TAB_STOP - 1) + 1);

  int idx = 0;
  for (int j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      row->render[idx++] = ' ';
      while (idx % ROAMR_TAB_STOP != 0)
        row->render[idx++] = ' ';
    } else {
      row->render[idx++] = row->chars[j];
    }
  }
  row->render[idx] = '\0';
  row->rsize = idx;
}
/**
 * @brief Insert a new row
 *
 * @param s string
 * @param len string length
 */
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows)
    return;

  E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  editorUpdateRow(&E.row[at]);

  E.numrows++;
  E.dirty++;
}

/**
 * @brief free the row
 *
 * @param row row struct
 */
void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
}

/**
 * @brief delete the row
 *
 * @param at row idx
 */
void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows)
    return;

  editorFreeRow(&E.row[at]);
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
  E.numrows--;
  E.dirty++;
}

/**
 * @brief insert a character to a row
 *
 * @param row editor row
 * @param at row idx
 * @param c characters
 */
void editorRowInsertChar(erow *row, int at, int c) {
  if (at < 0 || at > row->size)
    at = row->size;

  row->chars = realloc(row->chars, row->size + 2);
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
  row->size++;
  row->chars[at] = c;
  editorUpdateRow(row);
  E.dirty++;
}

/**
 * @brief Append a string to the end of a row
 *
 * @param row row to append
 * @param s string
 * @param len string length
 */
void editorRowAppendString(erow *row, char *s, size_t len) {
  row->chars = realloc(row->chars, row->size + len + 1);
  memcpy(&row->chars[row->size], s, len);
  row->size += len;
  row->chars[row->size] = '\0';
  editorUpdateRow(row);
  E.dirty++;
}

/**
 * @brief delete a character to a row
 *
 * @param row editor row
 * @param at row idx
 */
void editorRowDelChar(erow *row, int at) {
  if (at < 0 || at >= row->size)
    return;
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
  row->size--;
  editorUpdateRow(row);
  E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    editorInsertRow(E.numrows, "", 0);
  }
  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

void editorInsertNewline() {
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    erow *row = &E.row[E.cy];
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    row = &E.row[E.cy];
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
  }
  E.cy++;
  E.cx = 0;
}

/**
 * @brief delete a char
 */
void editorDelChar() {
  if (E.cy == E.numrows)
    return;
  if (E.cx == 0 && E.cy == 0)
    return;

  erow *row = &E.row[E.cy];
  if (E.cx > 0) {
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    E.cx = E.row[E.cy - 1].size;
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
    editorDelRow(E.cy);
    E.cy--;
  }
}

/*** file i/o ***/

/**
 * @brief converts array of erow struct into string.
 *
 * @param buflen buffer length
 * @return buffer
 */
char *editorRowsToString(int *buflen) {
  int totlen = 0;
  for (int i = 0; i < E.numrows; i++) {
    totlen += E.row[i].size + 1;
  }
  *buflen = totlen;

  char *buf = malloc(totlen);
  char *p = buf;
  for (int i = 0; i < E.numrows; i++) {
    memcpy(p, E.row[i].chars, E.row[i].size);
    p += E.row[i].size;
    *p = '\n';
    p++;
  }

  return buf;
}

/**
 * @brief open an a file
 *
 * @param filename filename
 */
void editorOpen(char *filename) {
  free(E.filename);
  E.filename = strdup(filename);

  FILE *fp = fopen(filename, "r");
  if (!fp)
    die("fopen");

  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    if (linelen != -1) {
      while (linelen > 0 &&
             (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
        linelen--;
      }

      editorInsertRow(E.numrows, line, linelen);
    }
  }
  free(line);
  fclose(fp);
  E.dirty = 0;
}

/**
 * @brief save to disk
 */
void editorSave() {
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s (ESC to cancel)");
    if (E.filename == NULL) {
      editorSetStatusMessage("Save aborted");
      return;
    }
  }

  int len;
  char *buf = editorRowsToString(&len);

  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    if (ftruncate(fd, len) != -1) {
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
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
 * @brief rowoff calculate
 */
void editorScroll() {
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
    return;
  }

  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
    return;
  }

  if (E.rx < E.coloff) {
    E.coloff = E.rx;
    return;
  }

  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
    return;
  }
}

/**
 * @brief Draw a column of tildes(~) on the left of the screen, like vim.
 *
 * @param ab write() the append buf contens out to standard
 */
void editorDrawRows(abuf *ab) {
  for (int i = 0; i < E.screenrows; i++) {
    int filerow = i + E.rowoff;
    if (filerow >= E.numrows) {
      if (E.numrows == 0 && i == E.screenrows / 3) {
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
    } else {
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].render[E.coloff], len);
    }

    abAppend(ab, "\x1b[K", 3);
    abAppend(ab, "\r\n", 2);
  }
}

/**
 * @brief draw status bar
 *
 * @param ab append buf
 */
void editorDrawStatusBar(abuf *ab) {
  abAppend(ab, "\x1b[7m", 4);

  char status[80], rstatus[80];
  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");
  int rlen = snprintf(rstatus, sizeof(status), "%d/%d", E.cy + 1, E.numrows);

  if (len > E.screencols)
    len = E.screencols;
  abAppend(ab, status, len);

  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }
  abAppend(ab, "\x1b[m", 3);
  abAppend(ab, "\r\n", 2);
}

/**
 * @brief draw status message
 *
 * @param ab append buf
 */
void editorDrawMessageBar(abuf *ab) {
  abAppend(ab, "\x1b[K", 3);
  int msglen = strlen(E.statusmsg);
  if (msglen > E.screencols)
    msglen = E.screencols;
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

/**
 * @brief Clear the screen
 */
void editorRefreshScreen() {
  editorScroll();

  abuf ab = ABUF_INIT;

  abAppend(&ab, "\x1b[?25l]h", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  char buf[32];

  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.cy - E.rowoff + 1,
           E.rx - E.coloff + 1);
  abAppend(&ab, buf, strlen(buf));

  abAppend(&ab, "\x1b[?25h", 6);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/**
 * @brief set editor status message
 *
 * @param fmt fmt
 */
void editorSetStatusMessage(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);
  E.statusmsg_time = time(NULL);
}

/*** input ***/

/**
 * @brief Prompt box
 *
 * @param prompt prompt fmt
 * @return user enterd string
 */
char *editorPrompt(char *prompt) {
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('d') || c == CTRL_KEY('h') ||
        c == BACKSPACE) {
      if (buflen != 0)
        buf[--buflen] = '\0';
    } else if (c == '\x1b') {
      // ESC
      editorSetStatusMessage("");
      free(buf);
      return NULL;
    } else if (c == '\r') {
      // ENTER
      if (buflen != 0) {
        editorSetStatusMessage("");
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      if (buflen == bufsize - 1) {
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      buf[buflen++] = c;
      buf[buflen] = '\0';
    }
  }
}

/**
 * @brief editor move cursor
 *
 * @param key key enum
 */
void editorMoveCursor(int key) {
  erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
  case ARROW_LEFT: {
    if (E.cx != 0) {
      E.cx--;
    } else if (E.cy > 0) {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  }
  case ARROW_DOWN: {
    if (E.cy < E.numrows) {
      E.cy++;
    }
    break;
  }
  case ARROW_UP: {
    if (E.cy != 0) {
      E.cy--;
    }
    break;
  }
  case ARROW_RIGHT: {
    if (row && E.cx < row->size) {
      E.cx++;
    } else if (row && E.cx == row->size) {
      E.cy++;
      E.cx = 0;
    }
    break;
  }
  }

  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    E.cx = rowlen;
  }
}

void editorProcessKeypress() {
  static int quit_times = ROAMR_QUIT_TIMES;

  int c = editorReadKey();

  switch (c) {
  case '\r':
    editorInsertNewline();
    break;

  case CTRL_KEY('q'): {
    if (E.dirty && quit_times > 0) {
      editorSetStatusMessage("WARNING: File has unsaved changes. "
                             "Press Ctrl-Q %d more times to quit.",
                             quit_times);
      quit_times--;
      return;
    }
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    exit(0);
    break;
  }

  case CTRL_KEY('s'):
    editorSave();
    break;

  case HOME_KEY:
    E.cx = 0;
    break;
  case END_KEY:
    if (E.cy < E.numrows)
      E.cx = E.row[E.cy].size;
    break;

  case BACKSPACE:
  case CTRL_KEY('h'):
    editorDelChar();
    break;

  case CTRL_KEY('d'):
  case DEL_KEY:
    editorMoveCursor(ARROW_RIGHT);
    editorDelChar();
    break;

  case PAGE_UP:
  case PAGE_DOWN: {
    if (c == PAGE_UP) {
      E.cy = E.rowoff;
    } else if (c == PAGE_DOWN) {
      E.cy = E.rowoff + E.screenrows - 1;
      if (E.cy > E.numrows)
        E.cy = E.numrows;
    }

    int times = E.screenrows;
    while (times--)
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    break;
  }

  case CTRL_KEY('f'):
    editorMoveCursor(ARROW_RIGHT);
    break;

  case CTRL_KEY('b'):
    editorMoveCursor(ARROW_LEFT);
    break;

  case CTRL_KEY('n'):
    editorMoveCursor(ARROW_DOWN);
    break;

  case CTRL_KEY('p'):
    editorMoveCursor(ARROW_UP);
    break;

  case ARROW_LEFT:
  case ARROW_DOWN:
  case ARROW_UP:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;

  case CTRL_KEY('l'):
  case '\x1b':
    // Escape: do nothing
    break;

  default:
    editorInsertChar(c);
    break;
  }

  quit_times = ROAMR_QUIT_TIMES;
}

/*** init ***/

/**
 * @brief initialize all the fields in the E struct
 */
void initEditor() {
  E.cx = 0;
  E.cy = 0;
  E.rx = 0;
  E.rowoff = 0;
  E.coloff = 0;
  E.numrows = 0;
  E.row = NULL;
  E.dirty = 0;
  E.filename = NULL;
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");

  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit");

  while (1) {
    editorRefreshScreen();
    editorProcessKeypress();
  }

  return EXIT_SUCCESS;
}
