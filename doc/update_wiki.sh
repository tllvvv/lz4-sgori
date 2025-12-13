set -euo pipefail

WIKI_REPO_URL=$(git remote get-url origin | sed 's/.git$/.wiki.git/g')

WIKI_DIR=$PWD/$(basename `git rev-parse --show-toplevel`).wiki
DOCS_DIR=$PWD/doc

cleanup() {
	rm -rf $WIKI_DIR
}

git clone $WIKI_REPO_URL

trap cleanup EXIT

cp -rf $DOCS_DIR/*.md $WIKI_DIR
cd $WIKI_DIR
git add .
git checkout

echo "Enter commit message: "
read -r commit_msg
git commit -m $commit_msg

while true; do
	echo "Are you sure you want to push to remote repository? [y/n]"
	read -r response

	if [[ $response =~ ^(yes|y)$ ]]; then
		git push
		break
	elif [[ $response =~ ^(no|n)$ ]]; then
		echo "Cancelling..."
		break
	fi
done
