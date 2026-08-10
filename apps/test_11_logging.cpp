#include "tests/TestCases.h"
#include <iostream>
int main(int argc,char** argv){try{auto o=ydtest::parseArgs(argc,argv);return ydtest::runTest11Logging(o);}catch(const std::exception& e){std::cerr<<"ERROR: "<<e.what()<<std::endl;return 2;}}
