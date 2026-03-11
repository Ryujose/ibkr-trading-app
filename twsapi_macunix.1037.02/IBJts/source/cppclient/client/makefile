CXX=g++
CXXFLAGS=-pthread -Wall -Wno-switch -Wpedantic -Wno-unused-function -std=c++11 -shared -fPIC
ROOT_DIR=.
BASE_SRC_DIR=${ROOT_DIR}
PROTOBUF_DIR=$(BASE_SRC_DIR)/protobufUnix
INCLUDES=-I${ROOT_DIR} -I${PROTOBUF_DIR}
LIB_DIR=lib
LIB_NAME=libbid.so
TARGET=libTwsSocketClient.so

$(TARGET):
	$(CXX) $(CXXFLAGS) $(INCLUDES) $(BASE_SRC_DIR)/*.cpp ${PROTOBUF_DIR}/*.cc -L$(LIB_DIR) -l:$(LIB_NAME) -o$(TARGET) -lprotobuf

clean:
	rm -f $(TARGET) *.o

