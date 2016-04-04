/*  Boolector: Satisfiablity Modulo Theories (SMT) solver.
 *
 *  Copyright (C) 2013 Armin Biere.
 *
 *  All rights reserved.
 *
 *  This file is part of Boolector.
 *  See COPYING for more information on using this software.
 */
#ifndef TESTMC_H_INCLUDED
#define TESTMC_H_INCLUDED

void init_mc_tests (void);

void run_mc_tests (int argc, char **argv);

void finish_mc_tests (void);

#endif