#include "tests/TestCases.h"
#include <iostream>
int main(int argc,char** argv){try{auto o=ydtest::parseArgs(argc,argv);return ydtest::runTest072InvalidPrice(o);}catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<std::endl;return 2;}}
