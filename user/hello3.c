/* hello3.c — first self-contained ring 3 test program for `cc3`.
 *
 * Скомпилируйте прямо из CFOS:  cc3 sd0p1 hello3.c
 * Затем запустите:              run hello3.elf
 *
 * Runtime API (устанавливается cc3 вручную, prelude):
 *   s_puts(str)            — строка на консоль
 *   print_i64 / print_u64 / print_hex64
 *   getchar()              — блокирующее чтение клавиши
 *   getpid(), uptime()
 *   s_write(buf, n), s_read(buf, n)
 *   main() возвращает int — код выхода.
 */

int main(void) {
    unsigned long silly[4];
    silly[0] = 1;
    silly[1] = 2;
    silly[2] = 3;
    silly[3] = 4;

    unsigned long sum = 0;
    for (int i = 0; i < 4; i++) sum += silly[i];

    s_puts("cc3: hello from ring 3!\n");
    s_puts("pid:     ");  print_u64(getpid());   s_nl();
    s_puts("uptime:  ");  print_u64(uptime());  s_nl();
    s_puts("sum:     ");  print_u64(sum);       s_nl();
    s_puts("hex:     ");  print_hex64(0xDEADCAFE); s_nl();
    s_puts("echo mode, 'q' quits\n> ");

    for (;;) {
        int c = getchar();
        if (c < 0) break;
        if (c == 'q' || c == 'Q') { s_nl(); s_puts("bye!\n"); return 42; }
        char ch = (char)c;
        s_write(&ch, 1);
    }
    return 0;
}
