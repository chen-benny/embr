//
// Created by benny on 4/20/26.
//

#pragma once

// Entry of `embr push` subcommand
// Parses args, optionally registers with tracker, invokes run_push
int run_push_cli(int argc, char* argv[]);