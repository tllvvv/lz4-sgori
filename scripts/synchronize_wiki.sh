set -euo pipefail

WIKI_REPO_URL=$(git remote get-url origin | sed 's/.git$/.wiki.git/g')

WIKI_DIR=$PWD/$(basename `git rev-parse --show-toplevel`).wiki
DOCS_DIR=$PWD/doc

cleanup() {
	rm -rf $WIKI_DIR
}

git clone $WIKI_REPO_URL

trap cleanup EXIT

rsync --recursive --delete --no-links --exclude '.git' $DOCS_DIR/ $WIKI_DIR/
cd $WIKI_DIR
ls -al
git add --all
git checkout

if ! git diff --quiet || git diff --cached --quiet; then
	echo "No changes found, exiting..."
	exit
fi

echo "Enter commit message: "
read -r commit_msg
git commit -m "$commit_msg"

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
