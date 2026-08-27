#include "tests/TestCases.h"
#include <iostream>

int main(int argc,char** argv){
    try{
        auto options=ydtest::parseArgs(argc,argv);
        return ydtest::runTest13SellAg2610(options);
    }catch(const std::exception& error){
        std::cerr<<"ERROR: "<<error.what()<<std::endl;
        return 2;
    }
}
