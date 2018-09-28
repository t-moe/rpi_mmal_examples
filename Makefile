
BINS_C= $(patsubst %.c, %, $(wildcard *.c))
BINS_CPP= $(patsubst %.cpp, %, $(wildcard *.cpp))

all: $(BINS_C) $(BINS_CPP) 


%: %.c
	gcc -I/opt/vc/include/ -I/opt/vc/include/interface/mmal $^ -o $@ -O0 -g -L/opt/vc/lib/ -lbcm_host -lmmal -lmmal_core -lmmal_components -lmmal_util -lvcos -lpthread 
%: %.cpp
	g++ -std=gnu++11 -Wall -W -D_REENTRANT  -fPIC -DQT_GUI_LIB -DQT_CORE_LIB -isystem /usr/include/arm-linux-gnueabihf/qt5 -isystem /usr/include/arm-linux-gnueabihf/qt5/QtGui -isystem /usr/include/arm-linux-gnueabihf/qt5/QtCore  -I/opt/vc/include/ -I/opt/vc/include/interface/mmal $^ -o $@ -O0 -g -L/opt/vc/lib/ -lbcm_host -lmmal -lmmal_core -lmmal_components -lmmal_util -lvcos -lpthread  -lQt5Gui -lQt5Core -lGLESv2 
clean:
	rm -f $(BINS_C) $(BINS_CPP)

