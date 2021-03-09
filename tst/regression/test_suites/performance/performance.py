#========================================================================================
# AthenaPK - a performance portable block structured AMR MHD code
# Copyright (c) 2020-2021, Athena Parthenon Collaboration. All rights reserved.
# Licensed under the 3-clause BSD License, see LICENSE file for details
#========================================================================================
# (C) (or copyright) 2020. Triad National Security, LLC. All rights reserved.
#
# This program was produced under U.S. Government contract 89233218CNA000001 for Los
# Alamos National Laboratory (LANL), which is operated by Triad National Security, LLC
# for the U.S. Department of Energy/National Nuclear Security Administration. All rights
# in the program are reserved by Triad National Security, LLC, and the U.S. Department
# of Energy/National Nuclear Security Administration. The Government is granted for
# itself and others acting on its behalf a nonexclusive, paid-up, irrevocable worldwide
# license in this material to reproduce, prepare derivative works, distribute copies to
# the public, perform publicly and display publicly, and to permit others to do so.
#========================================================================================

# Modules
import math
import numpy as np
import matplotlib
matplotlib.use('agg')
import matplotlib.pylab as plt
import sys
import os
import utils.test_case

""" To prevent littering up imported folders with .pyc files or __pycache_ folder"""
sys.dont_write_bytecode = True

perf_cfgs = [
    {"mx" : 256, "mb" : 256, "use_scratch" : False , "integrator" : "vl2", "recon" : "plm"},
    {"mx" : 256, "mb" : 256, "use_scratch" : True  , "integrator" : "vl2", "recon" : "plm"},
    {"mx" : 256, "mb" : 128, "use_scratch" : False , "integrator" : "vl2", "recon" : "plm"},
    {"mx" : 256, "mb" : 128, "use_scratch" : True , "integrator" : "vl2", "recon" : "plm"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : False , "integrator" : "vl2", "recon" : "plm"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : True , "integrator" : "vl2", "recon" : "plm"},
    {"mx" : 256, "mb" : 256, "use_scratch" : False , "integrator" : "rk2", "recon" : "plm"},
    {"mx" : 256, "mb" : 256, "use_scratch" : True  , "integrator" : "rk2", "recon" : "plm"},
    {"mx" : 256, "mb" : 128, "use_scratch" : False , "integrator" : "rk2", "recon" : "plm"},
    {"mx" : 256, "mb" : 128, "use_scratch" : True , "integrator" : "rk2", "recon" : "plm"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : False , "integrator" : "rk2", "recon" : "plm"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : True , "integrator" : "rk2", "recon" : "plm"},
    {"mx" : 256, "mb" : 256, "use_scratch" : False , "integrator" : "rk1", "recon" : "dc"},
    {"mx" : 256, "mb" : 256, "use_scratch" : True  , "integrator" : "rk1", "recon" : "dc"},
    {"mx" : 256, "mb" : 128, "use_scratch" : False , "integrator" : "rk1", "recon" : "dc"},
    {"mx" : 256, "mb" : 128, "use_scratch" : True , "integrator" : "rk1", "recon" : "dc"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : False , "integrator" : "rk1", "recon" : "dc"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : True , "integrator" : "rk1", "recon" : "dc"},
    {"mx" : 256, "mb" : 256, "use_scratch" : True , "integrator" : "rk3", "recon" : "ppm"},
    {"mx" : 256, "mb" : 128, "use_scratch" : True , "integrator" : "rk3", "recon" : "ppm"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : True , "integrator" : "rk3", "recon" : "ppm"},
    {"mx" : 256, "mb" : 256, "use_scratch" : True , "integrator" : "rk3", "recon" : "wenoz"},
    {"mx" : 256, "mb" : 128, "use_scratch" : True , "integrator" : "rk3", "recon" : "wenoz"},
    {"mx" : 256, "mb" : 64 , "use_scratch" : True , "integrator" : "rk3", "recon" : "wenoz"},
]

class TestCase(utils.test_case.TestCaseAbs):
    def Prepare(self,parameters, step):
        mx = perf_cfgs[step - 1]["mx"]
        mb = perf_cfgs[step - 1]["mb"]
        integrator = perf_cfgs[step - 1]["integrator"]
        use_scratch = perf_cfgs[step - 1]["use_scratch"]
        recon = perf_cfgs[step - 1]["recon"]

        parameters.driver_cmd_line_args = [
            'parthenon/mesh/nx1=%d' % mx,
            'parthenon/meshblock/nx1=%d' % mb,
            'parthenon/mesh/nx2=%d' % mx,
            'parthenon/meshblock/nx2=%d' % mb,
            'parthenon/mesh/nx3=%d' % mx,
            'parthenon/meshblock/nx3=%d' % mb,
            'parthenon/mesh/nghost=%d' % (3 if (recon == "ppm" or recon == "wenoz") else 2),
            'parthenon/mesh/refinement=none',
            'parthenon/time/integrator=%s' % integrator,
            'parthenon/time/nlim=10',
            'hydro/reconstruction=%s' % recon,
            'hydro/use_scratch=%s' % ("true" if use_scratch else "false"),
            ]

        return parameters

    def Analyse(self,parameters):

        perfs = []
        for output in parameters.stdouts:
            for line in output.decode("utf-8").split('\n'):
                print(line)
                if 'zone-cycles/wallsecond' in line:
                    perfs.append(float(line.split(' ')[2]))

        perfs = np.array(perfs)

        # Plot results
        fig, p = plt.subplots(2, 1, figsize = (4,8.0/10 * len(perf_cfgs)), sharey=True)
        labels = []

        for i, cfg in enumerate(perf_cfgs):
            p[0].plot(perfs[i]/1e6,i,'o')
            p[1].plot(perfs[i]/perfs[0],i,'o')
            labels.append((
                f'{cfg["integrator"].upper()} {cfg["recon"].upper()} '
                f'Scr: {"T" if cfg["use_scratch"] else "F"} Mesh ${cfg["mx"]}^3$ MB ${cfg["mb"]}^3$'
                ))

        p[0].set_xlabel("Mzone-cycles/s")
        p[1].set_xlabel("zcs normalized to bottom row")

        for i in range(2):
            p[i].grid()
            p[i].set_yticks(np.arange(len(perf_cfgs)))
            p[i].set_yticklabels(labels)

        fig.savefig(os.path.join(parameters.output_path, "performance.png"),
                    bbox_inches='tight')

        return True