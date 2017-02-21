
/*
 OSMScout - a Qt backend for libosmscout and libosmscout-map
 Copyright (C) 2016  Lukas Karas

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */


#ifndef MAPOBJECTINFOMODEL_H
#define MAPOBJECTINFOMODEL_H

#include <QObject>
#include <QAbstractListModel>

#include <osmscout/GeoCoord.h>
#include <osmscout/util/GeoBox.h>

#include <osmscout/DBThread.h>

#include <osmscout/LocationInfoModel.h>
#include <osmscout/private/ClientQtImportExport.h>

/**
 * \ingroup QtAPI
 */
class OSMSCOUT_CLIENT_QT_API MapObjectInfoModel: public QAbstractListModel
{
  Q_OBJECT
  Q_PROPERTY(bool ready READ isReady NOTIFY readyChange)

public:
  enum Roles {
      LabelRole = Qt::UserRole,
      TypeRole  = Qt::UserRole+1,
      IdRole    = Qt::UserRole+2,
      NameRole  = Qt::UserRole+3,
  };

signals:
  void readyChange(bool ready);
  void objectsRequested(const RenderMapRequest &view);

public slots:
  void dbInitialized(const DatabaseLoadedResponse&);
  void setPosition(QObject *mapView,
                   const int width, const int height,
                   const int screenX, const int screenY);
  void onViewObjectsLoaded(const RenderMapRequest&, const osmscout::MapData&);

private:
  void update();

  template<class T> void fillObjectInfo(QString type, const T &o)
  {
    //std::cout << " - "<<type.toStdString()<<": " << o->GetType()->GetName() << " " << o->GetObjectFileRef().GetFileOffset();
    QMap<int, QVariant> info;
    info[LabelRole]=QString::fromStdString(o->GetType()->GetName());
    info[TypeRole]=type;
    info[IdRole]=QVariant::fromValue<uint64_t>(o->GetObjectFileRef().GetFileOffset());

    const osmscout::FeatureValueBuffer &features=o->GetFeatureValueBuffer();
    const osmscout::NameFeatureValue *name=features.findValue<osmscout::NameFeatureValue>();
    if (name!=NULL){
      info[NameRole]=QString::fromStdString(name->GetLabel());
      //std::cout << " \"" << name->GetLabel() << "\"";
    }
    //std::cout << std::endl;
    model << info;
  }

public:
  MapObjectInfoModel();
  virtual inline ~MapObjectInfoModel(){};

  Q_INVOKABLE virtual int inline rowCount(const QModelIndex &/*parent = QModelIndex()*/) const
  {
      return model.size();
  };

  bool inline isReady() const
  {
      return ready;
  };

  Q_INVOKABLE virtual QVariant data(const QModelIndex &index, int role) const;
  virtual QHash<int, QByteArray> roleNames() const;
  Q_INVOKABLE virtual Qt::ItemFlags flags(const QModelIndex &index) const;

private:
  bool ready;
  bool setup;
  QList<ObjectKey> objectSet; // set of objects already inserted to model
  QList<QMap<int, QVariant>> model;
  RenderMapRequest view;
  int screenX;
  int screenY;
  QList<osmscout::MapData> mapData;
  double mapDpi;
};

#endif /* MAPOBJECTINFOMODEL_H */

