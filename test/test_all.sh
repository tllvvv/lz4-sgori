set -euxo pipefail

print_result() {
	exit_code=$?
	if [ $exit_code -eq 0 ]
	then
		echo -e '\e[32mSUCCESS!\e[0m'
	else
		echo -e '\e[31mFAILED!\e[0m'
	fi
	exit $exit_code
}

trap print_result EXIT

sh test/bash_tests/test_all.sh
sh test/fio_tests/test_all.sh
