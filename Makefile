BIN_DIR := ./bin/
CP      := \cp -f
RM      := \rm -f

all: FNodeMaster.aplx FNodeWorker.aplx IONode.aplx
	@if [ -f FNode/FNodeMaster/FNodeMaster.aplx ]; then $(CP) FNode/FNodeMaster/FNodeMaster.aplx $(BIN_DIR); echo "Copying FNodeMaster.aplx to bin..."; fi
	@if [ -f FNode/FNodeWorker/FNodeWorker.aplx ]; then $(CP) FNode/FNodeWorker/FNodeWorker.aplx $(BIN_DIR); echo "Copying FNodeWorker.aplx to bin..."; fi
	@if [ -f IONode/IONode.aplx ]; then $(CP) IONode/IONode.aplx $(BIN_DIR); "Copying IONode.aplx to bin..."; fi
	@echo "All is done!"

FNodeMaster.aplx:
	@echo "[spinFG] Building the FNodeMaster..."
	@cd FNode/FNodeMaster ; make ; make tidy
	@echo

FNodeWorker.aplx:
	@echo "[spinFG] Building the FNodeWorker..."
	@cd FNode/FNodeWorker ; make ; make tidy
	@echo

IONode.aplx:
	@echo "[spinFG] Building the IONode..."
	@cd IONode ; make ; make tidy
	@echo

clean:
	@cd FNode/FNodeMaster ; make clean
	@cd FNode/FNodeWorker ; make clean
	@cd IONode ; make clean
