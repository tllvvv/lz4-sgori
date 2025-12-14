#! /bin/bash

set -euxo pipefail

./test/bash_tests/test_init.sh
./test/bash_tests/test_info.sh
./test/bash_tests/test_proxy.sh
./test/bash_tests/test_stats.sh
