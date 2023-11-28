all:
	gcc -pthread  -D_GNU_SOURCE -c -o libUMS.o libUMS.c
	gcc -pthread  -D_GNU_SOURCE -c -o example.o example.c

	gcc -pthread  -D_GNU_SOURCE libUMS.o example.o -o example 

clean:
	rm -rfv *.o 
	rm -rfv example