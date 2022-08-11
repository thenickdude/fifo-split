.PHONY: all clean again

SUBMODULES = deps/UnitConvert/CMakeLists.txt

all : build/fifo-split

$(SUBMODULES) :
	git submodule update --init --recursive

build/fifo-split : *.cpp *.hpp $(SUBMODULES)
	mkdir -p build
	cd build && conan install --build=boost --profile=../conanprofile .. && cmake .. && cmake --build .

again:
	cd build && cmake --build .

clean :
	rm -rf build

