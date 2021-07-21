# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
#
# This file is part of the LibreOffice project.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
#

$(eval $(call gb_ExternalPackage_ExternalPackage,fonts_graphikarabic,font_graphikarabic))

$(eval $(call gb_ExternalPackage_add_unpacked_files,fonts_graphikarabic,$(LIBO_SHARE_FOLDER)/fonts/truetype,\
        Graphik_Arabic_Black.ttf \
        Graphik_Arabic_Bold.ttf \
        Graphik_Arabic_ExtraLight.ttf \
        Graphik_Arabic_Light.ttf \
        Graphik_Arabic_Medium.ttf \
        Graphik_Arabic_SemiBold.ttf \
        Graphik_Arabic_Super.ttf \
        Graphik_Arabic_Thin.ttf \
        Graphik_Arabic.ttf \
))

# vim: set noet sw=4 ts=4: