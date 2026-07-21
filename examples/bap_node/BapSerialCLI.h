#pragma once

#include <Arduino.h>
#include "BapConfig.h"

class MyMesh;

class BapSerialCLI {
  BapConfig* _cfg;
  MyMesh* _mesh;
  char _cmd_buf[256];
  size_t _cmd_len = 0;

public:
  BapSerialCLI(BapConfig& cfg, MyMesh& mesh) : _cfg(&cfg), _mesh(&mesh) {}

  // Process any serial input. Called from main loop().
  void loop();

  // Print prompt
  void prompt();

  // Print help
  void help();

private:
  void dispatch_(char* line);
  void cmdRole_(const char* arg);
  void cmdWifi_(const char* arg);
  void cmdEndpoint_(const char* arg);
  void cmdSecret_(const char* arg);
  void cmdKeygen_(const char* arg);
  void cmdPubkey_(const char* arg);
  void cmdStopsAdd_(const char* arg);
  void cmdStopsDel_(const char* arg);
  void cmdStopsList_();
  void cmdStop_(const char* arg);
  void cmdPoll_(const char* arg);
  void cmdSave_();
  void cmdShow_();
  void reply_(const char* msg);
  void reply_(const String& msg);

  // Quote-aware tokenizer: returns next whitespace-separated token, honoring "double quotes".
  static char* nextToken_(char** saveptr);
};
