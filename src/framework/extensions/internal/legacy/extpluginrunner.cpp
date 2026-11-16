/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-CLA-applies
 *
 * MuseScore
 * Music Composition & Notation
 *
 * Copyright (C) 2024 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "extpluginrunner.h"

#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>

#include "../../api/v1/ipluginapiv1.h"
#include "../../extensionserrors.h"

#include "containers.h"

#include "log.h"

using namespace muse;
using namespace muse::extensions;
using namespace muse::extensions::legacy;
using namespace muse::extensions::apiv1;

//! NOTE Plugins without UI stay alive after running (e.g. to serve a WebSocket).
//! They are remembered, so that running one again replaces it, with its QML file read anew.
static std::map<io::path_t, QPointer<QObject> > s_runningPlugins;

static Ret createPlugin(QQmlEngine* qmlEngine, const Action& action, int restart = 0)
{
    io::path_t qmlPath = action.path;

    //! NOTE The engine caches each file's component by URL, and keeps it while anything of the previous
    //! instance is still referenced (e.g. its callbacks in an API object). A restarted plugin is loaded
    //! under its own URL, so that it is read anew from the file.
    QUrl url = QUrl::fromLocalFile(qmlPath.toQString());
    if (restart > 0) {
        url.setQuery(QString("restart=%1").arg(restart));
    }

    //! NOTE We create extension UI using a separate engine to control what we provide,
    //! making it easier to maintain backward compatibility and stability.
    QQmlComponent component = QQmlComponent(qmlEngine, url);
    if (!component.isReady()) {
        LOGE() << "Failed to load QML file: " << qmlPath;
        LOGE() << component.errorString();
        return make_ret(Err::ExtLoadError);
    }

    QObject* obj = component.create();
    if (!obj) {
        LOGE() << "Failed to create QML Object: " << qmlPath;
        return make_ret(Err::ExtLoadError);
    }

    IPluginApiV1* plugin = dynamic_cast<IPluginApiV1*>(obj);
    if (!plugin) {
        LOGE() << "Qml Object not MuseScore plugin: " << qmlPath;
        delete obj;
        return make_ret(Err::ExtBadFormat);
    }

    s_runningPlugins[qmlPath] = obj;

    plugin->runPlugin();

    return muse::make_ok();
}

Ret ExtPluginRunner::run(const Action& action)
{
    QQmlEngine* qmlEngine = engine()->qmlEngineApiV1();
    QPointer<QObject> previous = muse::value(s_runningPlugins, action.path);
    if (!previous) {
        return createPlugin(qmlEngine, action);
    }

    //! NOTE The previous instance may be the caller (a plugin restarting itself), so it is deleted
    //! only after the current event, and the new instance is created once it is gone.
    static int restarts = 0;
    int restart = ++restarts;
    LOGI() << "Restarting plugin: " << action.path;
    QObject::connect(previous, &QObject::destroyed, qmlEngine, [qmlEngine, action, restart]() {
        qmlEngine->trimComponentCache();
        createPlugin(qmlEngine, action, restart);
    }, Qt::QueuedConnection);
    previous->deleteLater();

    return muse::make_ok();
}
