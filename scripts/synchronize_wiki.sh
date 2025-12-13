set -euo pipefail

WIKI_REPO_URL=$(git remote get-url origin | sed 's/.git$/.wiki.git/g')

WIKI_DIR=$PWD/$(basename `git rev-parse --show-toplevel`).wiki
DOCS_DIR=$PWD/doc

cleanup() {
	rm -rf $WIKI_DIR
}

remove_headers() {
	for page in $WIKI_DIR/*.md; do
		tail -n +3 $page > $page.tmp && mv $page.tmp $page
	done
}

correct_relative_links() {
	for page in $WIKI_DIR/*.md; do
		sed -i -E 's|\(([^:]*)\.md\)|(\1)|g; s|\(([^:]*)\.md#|(\1#|g' $page
	done
}

git clone $WIKI_REPO_URL

trap cleanup EXIT

rsync --recursive --delete --no-links --exclude '.git' $DOCS_DIR/ $WIKI_DIR/
cd $WIKI_DIR
ls -al

remove_headers
correct_relative_links
git add --all
git checkout

if git diff --staged --exit-code; then
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
