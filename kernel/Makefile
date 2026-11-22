all: head.o
	objcopy -I elf64-x86-64 -S -R ".eh_frame" -R ".comment" -O binary head.o kernel.bin

haed.o: head.s 
	as --64 -o head.o head.s 

clean:
	rm -rf head.o kernel.bin