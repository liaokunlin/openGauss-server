#
# Copyright (c) 2020 Huawei Technologies Co.,Ltd.
#
# openGauss is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#
#          http://license.coscl.org.cn/MulanPSL2
#
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
# ---------------------------------------------------------------------------------------
#
# Makefile
#     Makefile for the pldebugger
#
# IDENTIFICATION
#        contrib/pldebugger/Makefile
#
# ---------------------------------------------------------------------------------------

all:pldebuger_target
install:install-data

top_builddir = ../../
PLDEBUGGER_DIR=$(top_builddir)/third_party/dependency/pldebugger
#source code
PLDEBUGGER_PACKAGE=pldebugger_3_0
PLDEBUGGER_PATCH=huawei_pldebugger
PLDEBUGGER_MEGRED_SOURCES_DIR=$(PLDEBUGGER_DIR)

.PHONY: pldebugger_target
pldebuger_target:
	@$(call create_pldebugger_sources)
	@make -C $(PLDEBUGGER_MEGRED_SOURCES_DIR)/$(PLDEBUGGER_PACKAGE)

.PHONY: install-data
install-data: pldebugger_target
	@make -C $(PLDEBUGGER_MEGRED_SOURCES_DIR)/$(PLDEBUGGER_PACKAGE) install

uninstall distclean clean:
	@rm -rf $(PLDEBUGGER_MEGRED_SOURCES_DIR)/$(PLDEBUGGER_PACKAGE)
define create_pldebugger_sources
        cd $(PLDEBUGGER_DIR); \
        sh patch.sh;  \
        cd - ;
endef
