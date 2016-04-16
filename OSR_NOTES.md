Links:

- Updated version of the paper: https://www.authorea.com/users/55853/articles/66046/
- Error: extern function not found: http://koichitamura.blogspot.com/2011/01/since-i-went-to-held-several-weeks-ago.html
- http://lowlevelbits.org/system-under-test-llvm/
- how does julia eval: https://github.com/JuliaLang/julia/blob/a6992dd4d2ca08601afaaabb55fd52cef5a76a76/doc/devdocs/eval.rst
- julia can use either Orc or McJIT: https://github.com/JuliaLang/julia/blob/c29bff1462cf17e8c8d7a357d191d939d4eca650/src/codegen.cpp#L88 (or the old one)
- on https://github.com/JuliaLang/julia/blob/c29bff1462cf17e8c8d7a357d191d939d4eca650/src/codegen.cpp#L967 we pass in an llvm function, the get the function to call back from whatever code generator we are using
- when codegen is complete, the function https://github.com/JuliaLang/julia/blob/c29bff1462cf17e8c8d7a357d191d939d4eca650/src/codegen.cpp#L937 is called, kicking off the compilation of a module. (I'm not sure if this module is a module specific to the function or if it is a module for)
- flex thing: https://github.com/JuliaLang/julia/blob/c29bff1462cf17e8c8d7a357d191d939d4eca650/src/toplevel.c#L422 seems to be the starting point for all possible evaluation
- actual call happens here: https://github.com/JuliaLang/julia/blob/c29bff1462cf17e8c8d7a357d191d939d4eca650/src/julia\_internal.h#L62
- looks to me like this is really similar to what the kalidescope example does, just with more complexity for the julia stuff.
- terra does about the same thing: https://github.com/zdevito/terra/blob/b8ede26d03a551ea63f662290234062d0cde1e07/src/tcompiler.cpp#L322
- Looks like we can be pretty safe if we assume that kalidescope is a good baseline for normal behavior in a program (it just might be spread out a bunch)


BUILD WITH THIS, else the tool doesn't work:

```
cmake -G Ninja -DLLVM_ENABLE_FFI=true -DCMAKE_BUILD_TYPE=Debug ../
```
