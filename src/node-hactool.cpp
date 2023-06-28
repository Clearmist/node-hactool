#include <iostream>
#include <string>
#include <vector>
#include <napi.h>
#define FMT_HEADER_ONLY
#include <fmt/core.h>
#include "hactool/main.h"

#pragma message("Importing the Node API wrapper")

using namespace Napi;

char **BuildParameters(const CallbackInfo& info) {
  char** parameters = new char*[info.Length()];

  for (int i = 0; i < info.Length(); i = i + 1) {
    std::string str = info[i].ToString().Utf8Value();

    // Copy the string to char before adding it to the parameter array.
    char * actualString = new char[str.length() + 1];
    std::strcpy(actualString, str.c_str());

    parameters[i] = actualString;
  }

  return parameters;
}

String Run(const CallbackInfo& info) {
  return String::New(info.Env(), start(info.Length(), BuildParameters(info), info.Env()));
}

Object InitAll(Env env, Object exports) {
  exports.Set("run", Function::New(env, Run));

  return exports;
}

NODE_API_MODULE(NODE_GYP_MODULE_NAME, InitAll);
