
SRC_FILES := $(wildcard *.c)
BINS= $(patsubst %.c, %, $(SRC_FILES))

all: $(BINS) 


%: %.c
	gcc -I/opt/vc/include/ -I/opt/vc/include/interface/mmal $^ -o $@ -O0 -g -L/opt/vc/lib/ -lbcm_host -lmmal -lmmal_core -lmmal_components -lmmal_util -lvcos -lpthread 
clean:
	rm -f $(BINS)

