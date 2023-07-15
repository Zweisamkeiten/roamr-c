/*** include ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

/*** define ***/

#define CTRL_KEY(k) ((k)&0x1f)

/*** data ***/

struct termios orig_termios;

/*** terminal ***/

/**
 * @brief Error handling
 *
 * @param s error message
 */
void die(const char *s) {
  // perror looks at the global `errno` and print *s before error message.
  perror(s);
  exit(EXIT_FAILURE);
}

/**
 * @brief Disable raw mode at exit
 */
void disableRawMode() {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios) == -1)
    die("tcsetattr");
}

/**
 * @brief Turn off echoing
 */
void enableRawMode() {
  if (tcgetattr(STDIN_FILENO, &orig_termios) == -1)
    die("tcsetattr");
  atexit(disableRawMode);

  struct termios raw = orig_termios;

  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
  raw.c_oflag &= ~(OPOST);
  raw.c_cflag |= (CS8);
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

/*** init ***/

int main(int argc, char *argv[]) {
  enableRawMode();

  while (1) {
    char c = '\0';
    // when read timeout return -1 & errno EAGAIN
    if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN)
      die("read");

    if (iscntrl(c)) {
      printf("%d\r\n", c);
    } else {
      printf("%d ('%c')\r\n", c, c);
    }

    if (c == CTRL_KEY('q'))
      break;
  }

  return EXIT_SUCCESS;
}
