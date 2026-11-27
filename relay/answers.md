# Answers from the maintainer

## M-1 (2026-11-27 19:58 UTC, from the cloud session: the maintainer's HaikuPorter setup)
On his Renga machine the trees are ~/ports/haikuports (HaikuPorter's TREE_PATH) and ~/ports/RengaPorts. HaikuPorter reads only the haikuports tree, so a RengaPorts recipe is copied over it before building, and a stale work directory must be deleted or the new patches are skipped. Home is /boot/home (not /home/<user>). Built packages land in ~/ports/haikuports/packages. Write native-build instructions in this form:
  cd ~/ports/RengaPorts && git fetch -q origin && git checkout <branch>
  cp -r <category>/<port>/. ../haikuports/<category>/<port>/
  cd ../haikuports && rm -rf <category>/<port>/work-<version>
  haikuporter -j12 --get-dependencies <port>-<version>
Test by extracting into ~/<name>test (package extract -C) and LIBRARY_PATH; publish with tools/update-repo.sh from ~/ports/RengaPorts (switch back to main first).
