// Public domain. To the extent possible under law, Fred Bulback has waived all copyright and related or neighboring rights to URL Encoding/Decoding in C. This work is published from: United States. Source http://www.geekhideout.com/urlcode.shtml.

char from_hex(char ch);
char to_hex(char code);
char *url_encode(char *str);
char *url_decode(char *str);