CFLAGS=-g -Wall -pipe -O3
CFLAGS+=`pkg-config --cflags sdl2`
LFLAGS+=`pkg-config --libs sdl2` -lSDL2_image
save_images: save_images.c
	gcc -o save_images save_images.c $(CFLAGS) $(LFLAGS)

clean:
	rm -f save_images
