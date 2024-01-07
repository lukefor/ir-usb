# IR-USB

Simple application to communicate to IR Tiqiaa Tview 10c4:8468

Based on the reverse-engineering article from XEN: https://habr.com/ru/post/494800/

## Usage

The application is terminal based, so you need to use arguments in order to make it work.

This command will capture the signal via IR receiver, write it to signal.bin file, after that read
this file and send it via IR transmitter.
```
$ ./ir-usb -r signal.bin -s signal.bin
```

All the commands will be executed sequentially, so you can have quite long list of `-r` and `-s`
with the corresponding files.
