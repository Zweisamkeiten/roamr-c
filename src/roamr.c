#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

/**
 * @brief Disable raw mode at exit
 */
void disableRawMode() { tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios); }

/**
 * @brief Turn off echoing
 */
void enableRawMode() {
  tcgetattr(STDIN_FILENO, &orig_termios);
  atexit(disableRawMode);

  struct termios raw = orig_termios;

  raw.c_lflag &= ~(ECHO | ICANON);

  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

int main(int argc, char *argv[]) {
  enableRawMode();

  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q')
    ;

  return EXIT_SUCCESS;
}
