#! /bin/bash

set -euo pipefail

WIKI_REPO_URL=$(git remote get-url origin | sed 's/\.git$\|$/.wiki.git/g')
WIKI_REPO_NAME=$(git rev-parse --show-toplevel | xargs basename).wiki

WIKI_DIR=$PWD/$WIKI_REPO_NAME
DOCS_DIR=$PWD/doc

CLONE_WIKI=1
COMMIT_MESSAGE=""
BASE_COMMIT_SHA=""
ASSUME_YES=0

cleanup() {
	rm -rf "$WIKI_DIR"
}

remove_headers() {
	for page in "$WIKI_DIR"/*.md; do
		tail -n +3 "$page" > "$page".tmp && mv "$page".tmp "$page"
	done
}

correct_relative_links() {
	for page in "$WIKI_DIR"/*.md; do
		sed -i -E 's|\(([^:]*)\.md\)|(\1)|g; s|\(([^:]*)\.md#|(\1#|g' "$page"
	done
}

while [[ $# -gt 0 ]]; do
	case $1 in
	-w|--wiki-path)
		WIKI_DIR=$(realpath "$2")
		CLONE_WIKI=0
		shift
		shift
		;;
	-m|--message)
		COMMIT_MESSAGE="$2"
		shift
		shift
		;;
	-b|--base-commit)
		BASE_COMMIT_SHA="$2"
		shift
		shift
		;;
	-y|--assume-yes)
		ASSUME_YES=1
		shift
		;;
	*)
		echo "Unknown option argument: $1"
		exit 1
		;;
	esac
done

if [ "$BASE_COMMIT_SHA" ] && [ ! "$COMMIT_MESSAGE" ]; then
	COMMIT_MESSAGE=$(echo -e "wiki sync\n\nSynchronize after commits:\n$(git log "$BASE_COMMIT_SHA"..HEAD --pretty=format:'- %s')")
fi

if [ "$CLONE_WIKI" -eq 1 ]; then
	git clone "$WIKI_REPO_URL" "$WIKI_DIR"
	trap cleanup EXIT
fi

if [ ! -d "$WIKI_DIR" ]; then
	echo "Path doesn't exist: $WIKI_DIR"
	exit 2
fi

if [ "$(cd "$WIKI_DIR"; git remote get-url origin)" != "$WIKI_REPO_URL" ]; then
	echo "Invalid wiki repository: $WIKI_DIR"
	exit 2
fi

rsync --recursive --delete --no-links --exclude '.git' "$DOCS_DIR"/ "$WIKI_DIR"/
cd "$WIKI_DIR"
ls -al

remove_headers
correct_relative_links
git add --all
git checkout

if git -P diff --staged --exit-code; then
	echo "No changes found, exiting..."
	exit 0
fi

while [ ! "$COMMIT_MESSAGE" ]; do
	echo "Enter commit message: "
	read -r COMMIT_MESSAGE
done

git commit -m "$COMMIT_MESSAGE"

while [ $ASSUME_YES -eq 0 ]; do
	echo "Are you sure you want to push to remote repository? [y/n]"
	read -r response

	if [[ $response =~ ^(y|yes)$ ]]; then
		break
	elif [[ $response =~ ^(n|no)$ ]]; then
		echo "Cancelling..."
		exit 0
	fi
done

git push
