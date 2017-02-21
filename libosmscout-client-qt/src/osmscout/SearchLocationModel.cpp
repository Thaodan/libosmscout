/*
 OSMScout - a Qt backend for libosmscout and libosmscout-map
 Copyright (C) 2010  Tim Teulings
 Copyright (C) 2016  Lukáš Karas

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

#include <iostream>

#include <osmscout/DBThread.h>
#include <osmscout/SearchLocationModel.h>
    
LocationListModel::LocationListModel(QObject* parent)
: QAbstractListModel(parent), searching(false)
{
    DBThread *dbThread = DBThread::GetInstance();
    
    qRegisterMetaType<QList<LocationEntry>>("QList<LocationEntry>");
    
    connect(this, SIGNAL(SearchRequested(const QString, int)), 
            dbThread, SLOT(SearchForLocations(const QString, int)), 
            Qt::QueuedConnection);
    
    connect(dbThread, SIGNAL(searchResult(const QString, const QList<LocationEntry>)),
            this, SLOT(onSearchResult(const QString, const QList<LocationEntry>)),
            Qt::QueuedConnection);
    
    connect(dbThread, SIGNAL(searchFinished(const QString, bool)),
            this, SLOT(onSearchFinished(const QString, bool)),
            Qt::QueuedConnection);
}

LocationListModel::~LocationListModel()
{
    for (QList<LocationEntry*>::iterator location=locations.begin();
         location!=locations.end();
         ++location) {
        delete *location;
    }

    locations.clear();
}

void LocationListModel::onSearchResult(const QString searchPattern, 
                                       const QList<LocationEntry> foundLocations)
{
  if (this->pattern.isEmpty()) {
      return; //No search requested
  }
  if (!searchPattern.contains(this->pattern, Qt::CaseInsensitive)){
    return; // result is not for us
  }
  
  for (auto &location : foundLocations) {
    // TODO: deduplicate, sort by relevance
    emit beginInsertRows(QModelIndex(), locations.size(), locations.size());
    locations.append(new LocationEntry(location));
    emit endInsertRows();
  }  
  emit countChanged(locations.size());
  qDebug() << "added " << foundLocations.size() << ", model size" << locations.size();
}

void LocationListModel::onSearchFinished(const QString searchPattern, bool /*error*/)
{
  if (this->lastRequestPattern!=searchPattern){
    return; // result is not for us
  }
  if (lastRequestPattern!=pattern){
    qDebug() << "Search postponed" << pattern;
    lastRequestPattern=pattern;
    emit SearchRequested(pattern, 50);
  }else{
    searching = false;
    emit SearchingChanged(false);
  }
}

void LocationListModel::setPattern(const QString& pattern)
{
  this->pattern = pattern;
  
  // remove old items
  // TODO: remove only invalid items that don't match to new pattern
  beginRemoveRows(QModelIndex(), 0, locations.size()-1);
  for (QList<LocationEntry*>::iterator location=locations.begin();
       location!=locations.end();
       ++location) {
      delete *location;
  }

  locations.clear();
  endRemoveRows();
  emit countChanged(locations.size());

  std::string stdPattern=pattern.toUtf8().constData();

  osmscout::GeoCoord coord;

  if (osmscout::GeoCoord::Parse(stdPattern, coord)) {
      QString name=QString::fromLocal8Bit(coord.GetDisplayText().c_str());
      QString label=name;

      LocationEntry *location=new LocationEntry(label, coord);
      beginInsertRows(QModelIndex(), locations.size(), locations.size());
      locations.append(location);
      endInsertRows();
  }
  emit countChanged(locations.size());

  if (searching){
    // we are still waiting for previous request, postpone current
    qDebug() << "Clear (" << locations.size() << ") postpone search" << pattern;
    return;
  }
  
  qDebug() << "Clear (" << locations.size() << ") search" << pattern;
  searching = true;
  lastRequestPattern = pattern;
  emit SearchingChanged(true);
  emit SearchRequested(pattern, 50);
}

int LocationListModel::rowCount(const QModelIndex& ) const
{
    return locations.size();
}

QVariant LocationListModel::data(const QModelIndex &index, int role) const
{
    if(index.row() < 0 || index.row() >= locations.size()) {
        return QVariant();
    }

    LocationEntry* location=locations.at(index.row());

    switch (role) {
    case Qt::DisplayRole:
    case LabelRole:
        return location->getLabel();
    case TypeRole:
        if (location->getType()==LocationEntry::typeCoordinate)
          return "coordinate";
        else 
          return location->getObjectType();
    case RegionRole:
        return location->getAdminRegionList();
    case LatRole:
        return QVariant::fromValue(location->getCoord().GetLat());
    case LonRole:
        return QVariant::fromValue(location->getCoord().GetLon());
    default:
        break;
    }

    return QVariant();
}

Qt::ItemFlags LocationListModel::flags(const QModelIndex &index) const
{
    if(!index.isValid()) {
        return Qt::ItemIsEnabled;
    }

    return QAbstractListModel::flags(index) | Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QHash<int, QByteArray> LocationListModel::roleNames() const
{
    QHash<int, QByteArray> roles=QAbstractListModel::roleNames();

    roles[LabelRole]="label";
    roles[TypeRole]="type";
    roles[RegionRole]="region";
    roles[LatRole]="lat";
    roles[LonRole]="lon";

    return roles;
}

LocationEntry* LocationListModel::get(int row) const
{
    if(row < 0 || row >= locations.size()) {
        return NULL;
    }

    LocationEntry* location=locations.at(row);
    // QML will take ownerhip
    return new LocationEntry(*location);
}
