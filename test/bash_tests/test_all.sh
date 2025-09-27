set -euxo pipefail

sh test/bash_tests/test_init.sh
sh test/bash_tests/test_info.sh
sh test/bash_tests/test_proxy.sh
sh test/bash_tests/test_stats.sh
