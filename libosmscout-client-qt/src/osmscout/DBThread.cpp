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

#include <osmscout/MapService.h>

#include <osmscout/DBThread.h>
#include <osmscout/TiledDBThread.h>
#include <osmscout/PlaneDBThread.h>
#include <osmscout/private/Config.h>
#include "osmscout/MapManager.h"
#ifdef OSMSCOUT_HAVE_LIB_MARISA
#include <osmscout/TextSearchIndex.h>
#endif

DBThread::DBThread(QStringList databaseLookupDirs,
                   QString iconDirectory,
                   SettingsRef settings)
  : mapManager(std::make_shared<MapManager>(databaseLookupDirs)),
    settings(settings),
    mapDpi(-1),
    physicalDpi(-1),
    lock(QReadWriteLock::Recursive),
    iconDirectory(iconDirectory),
    daylight(true),
    renderSea(true),
    fontName("sans-serif"),
    fontSize(2.0)
{
  // fix Qt signals with uint32_t on x86_64:
  //
  // QObject::connect: Cannot queue arguments of type 'uint32_t'
  // (Make sure 'uint32_t' is registered using qRegisterMetaType().)
  qRegisterMetaType < uint32_t >("uint32_t");

  // other types used in signals/slots
  qRegisterMetaType<QList<QDir>>("QList<QDir>");
  qRegisterMetaType<osmscout::GeoBox>("osmscout::GeoBox");

  QScreen *srn=QGuiApplication::screens().at(0);

  physicalDpi = (double)srn->physicalDotsPerInch();
  osmscout::log.Debug() << "Reported screen DPI: " << physicalDpi;
  mapDpi = settings->GetMapDPI();
  osmscout::log.Debug() << "Map DPI override: " << mapDpi;

  renderSea=settings->GetRenderSea();
  fontName=settings->GetFontName();
  fontSize=settings->GetFontSize();
  stylesheetFilename=settings->GetStyleSheetAbsoluteFile();
  stylesheetFlags=settings->GetStyleSheetFlags();
  osmscout::log.Debug() << "Using stylesheet: " << stylesheetFilename.toStdString();

  connect(settings.get(), SIGNAL(MapDPIChange(double)),
          this, SLOT(onMapDPIChange(double)),
          Qt::QueuedConnection);
  connect(settings.get(), SIGNAL(FontNameChanged(const QString)),
          this, SLOT(onFontNameChanged(const QString)),
          Qt::QueuedConnection);
  connect(settings.get(), SIGNAL(FontSizeChanged(double)),
          this, SLOT(onFontSizeChanged(double)),
          Qt::QueuedConnection);

  connect(mapManager.get(), SIGNAL(databaseListChanged(QList<QDir>)),
          this, SLOT(onDatabaseListChanged(QList<QDir>)),
          Qt::QueuedConnection);
}

DBThread::~DBThread()
{
  osmscout::log.Debug() << "DBThread::~TiledDBThread()";

  for (auto db:databases){
    db->close();
  }
  databases.clear();
}


bool DBThread::AssureRouter(osmscout::Vehicle vehicle)
{
  for (auto db:databases){
    if (!db->AssureRouter(vehicle, routerParameter)){
      return false;
    }
  }
  return true;
}

/**
 * check if DBThread is initialized without acquire mutex
 *
 * @return true if all databases are open
 */
bool DBThread::isInitializedInternal()
{
  for (auto db:databases){
    if (!db->database->IsOpen()){
      return false;
    }
  }
  return true;
}

bool DBThread::isInitialized(){
  QReadLocker locker(&lock);
  return isInitializedInternal();
}

double DBThread::GetMapDpi() const
{
    return mapDpi;
}

double DBThread::GetPhysicalDpi() const
{
    return physicalDpi;
}

const DatabaseLoadedResponse DBThread::loadedResponse() const {
  QReadLocker locker(&lock);
  DatabaseLoadedResponse response;
  for (auto db:databases){
    if (response.boundingBox.IsValid()){
      osmscout::GeoBox boundingBox;
      db->database->GetBoundingBox(boundingBox);
      response.boundingBox.Include(boundingBox);
    }else{
      db->database->GetBoundingBox(response.boundingBox);
    }
  }
  return response;
}

DatabaseCoverage DBThread::databaseCoverage(const osmscout::Magnification &magnification,
                                            const osmscout::GeoBox &bbox)
{
  QReadLocker locker(&lock);

  osmscout::GeoBox boundingBox;
  for (const auto &db:databases){
    if (boundingBox.IsValid()){
      osmscout::GeoBox dbBox;
      if (db->database->GetBoundingBox(dbBox)){
        boundingBox.Include(dbBox);
      }
    }else{
      db->database->GetBoundingBox(boundingBox);
    }
  }
  if (boundingBox.IsValid()) {
    /*
    qDebug() << "Database bounding box: " <<
                QString::fromStdString( boundingBox.GetDisplayText()) <<
            " test bounding box: " <<
                QString::fromStdString( tileBoundingBox.GetDisplayText() );
     */

    if (boundingBox.Includes(bbox.GetMinCoord()) &&
        boundingBox.Includes(bbox.GetMaxCoord())) {

      // test if some database has full coverage for this box
      bool fullCoverage=false;
      for (const auto &db:databases){
        std::list<osmscout::GroundTile> groundTiles;
        if (!db->mapService->GetGroundTiles(bbox,magnification,groundTiles)){
          break;
        }
        bool mayContainsUnknown=false;
        for (const auto &tile:groundTiles){
          if (tile.type==osmscout::GroundTile::unknown ||
              tile.type==osmscout::GroundTile::coast){
            mayContainsUnknown=true;
            break;
          }
        }
        if (!mayContainsUnknown){
          fullCoverage=true;
          break;
        }
      }

      return fullCoverage? DatabaseCoverage::Covered: DatabaseCoverage::Intersects;
    }
    if (boundingBox.Intersects(bbox)){
      return DatabaseCoverage::Intersects;
    }
  }
  return DatabaseCoverage::Outside;
}

void DBThread::TileStateCallback(const osmscout::TileRef& /*changedTile*/)
{

}

bool DBThread::InitializeDatabases()
{
  QReadLocker locker(&lock);
  qDebug() << "Initialize databases";
  mapManager->lookupDatabases();

  return true;
}

void DBThread::onDatabaseListChanged(QList<QDir> databaseDirectories)
{
  QWriteLocker locker(&lock);

  for (auto db:databases){
    db->close();
  }
  databases.clear();
  osmscout::GeoBox boundingBox;

#if defined(HAVE_MMAP)
  if (sizeof(void*)<=4){
    // we are on 32 bit system probably, we have to be careful with mmap
    qint64 mmapQuota=1.5 * (1<<30); // 1.5 GiB
    QStringList mmapFiles;
    mmapFiles << "bounding.dat" << "router2.dat" << "types.dat" << "textregion.dat" << "textpoi.dat"
              << "textother.dat" << "areasopt.dat" << "areanode.idx" << "textloc.dat" << "water.idx"
              << "areaway.idx" << "waysopt.dat" << "intersections.idx" << "router.idx" << "areaarea.idx"
              << "location.idx" << "intersections.dat";

    for (auto &databaseDirectory:databaseDirectories){
      for (auto &file:mmapFiles){
        mmapQuota-=QFileInfo(databaseDirectory, file).size();
      }
    }
    if (mmapQuota<0){
      qWarning() << "Database is too huge to be mapped";
    }

    qint64 nodesSize=0;
    for (auto &databaseDirectory:databaseDirectories){
      nodesSize+=QFileInfo(databaseDirectory, "nodes.dat").size();
    }
    if (mmapQuota-nodesSize<0){
      qWarning() << "Nodes data files can't be mmapped";
      databaseParameter.SetNodesDataMMap(false);
    }else{
      mmapQuota-=nodesSize;
    }

    qint64 areasSize=0;
    for (auto &databaseDirectory:databaseDirectories){
      areasSize+=QFileInfo(databaseDirectory, "areas.dat").size();
    }
    if (mmapQuota-areasSize<0){
      qWarning() << "Areas data files can't be mmapped";
      databaseParameter.SetAreasDataMMap(false);
    }else{
      mmapQuota-=areasSize;
    }

    qint64 waysSize=0;
    for (auto &databaseDirectory:databaseDirectories){
      waysSize+=QFileInfo(databaseDirectory, "ways.dat").size();
    }
    if (mmapQuota-waysSize<0){
      qWarning() << "Ways data files can't be mmapped";
      databaseParameter.SetWaysDataMMap(false);
    }else{
      mmapQuota-=waysSize;
    }

    qint64 routerSize=0;
    for (auto &databaseDirectory:databaseDirectories){
      routerSize+=QFileInfo(databaseDirectory, "router.dat").size();
    }
    if (mmapQuota-routerSize<0){
      qWarning() << "Router data files can't be mmapped";
      databaseParameter.SetRouterDataMMap(false);
    }
  }
#endif

  for (auto &databaseDirectory:databaseDirectories){
    osmscout::DatabaseRef database = std::make_shared<osmscout::Database>(databaseParameter);
    osmscout::StyleConfigRef styleConfig;
    if (database->Open(databaseDirectory.absolutePath().toLocal8Bit().data())) {
      osmscout::TypeConfigRef typeConfig=database->GetTypeConfig();

      if (typeConfig) {
        styleConfig=std::make_shared<osmscout::StyleConfig>(typeConfig);

        // setup flag overrides before load
        for (const auto& flag : stylesheetFlags) {
            styleConfig->AddFlag(flag.first,flag.second);
        }

        if (!styleConfig->Load(stylesheetFilename.toLocal8Bit().data())) {
          qDebug() << "Cannot load style sheet!";
          styleConfig=NULL;
        }
      }
      else {
        qDebug() << "TypeConfig invalid!";
        styleConfig=NULL;
      }
    }
    else {
      qWarning() << "Cannot open database!";
      continue;
    }

    if (!database->GetBoundingBox(boundingBox)) {
      qWarning() << "Cannot read initial bounding box";
      database->Close();
      continue;
    }

    osmscout::MapService::TileStateCallback callback=[this](const osmscout::TileRef& tile) {TileStateCallback(tile);};
    osmscout::MapServiceRef mapService = std::make_shared<osmscout::MapService>(database);
    osmscout::MapService::CallbackId callbackId=mapService->RegisterTileStateCallback(callback);

    databases << std::make_shared<DBInstance>(databaseDirectory.absolutePath(),
                                              database,
                                              std::make_shared<osmscout::LocationService>(database),
                                              mapService,
                                              callbackId,
                                              std::make_shared<QBreaker>(),
                                              styleConfig);
  }

  emit databaseLoadFinished(boundingBox);
  emit stylesheetFilenameChanged();
}

void DBThread::Finalize()
{
  QWriteLocker locker(&lock);
  qDebug() << "Finalize";

  for (auto db:databases){
    db->close();
  }
  databases.clear();
}

void DBThread::CancelCurrentDataLoading()
{
  for (auto db:databases){
    db->dataLoadingBreaker->Break();
  }
}

void DBThread::ToggleDaylight()
{
  {
    QWriteLocker locker(&lock);

    if (!isInitializedInternal()) {
        return;
    }
    qDebug() << "Toggling daylight from " << daylight << " to " << !daylight << "...";
    daylight=!daylight;
    stylesheetFlags["daylight"] = daylight;
  }

  ReloadStyle();

  qDebug() << "Toggling daylight done.";
}

void DBThread::SetStyleFlag(const QString &key, bool value)
{
  {
    QWriteLocker locker(&lock);

    if (!isInitializedInternal()) {
        return;
    }
    stylesheetFlags[key.toStdString()] = value;
  }
  ReloadStyle();
}

void DBThread::ReloadStyle(const QString &suffix)
{
  qDebug() << "Reloading style" << stylesheetFilename << suffix << "...";
  LoadStyle(stylesheetFilename, stylesheetFlags,suffix);
  qDebug() << "Reloading style done.";
}

void DBThread::LoadStyle(QString stylesheetFilename,
                         std::unordered_map<std::string,bool> stylesheetFlags,
                         const QString &suffix)
{
  QWriteLocker locker(&lock);

  this->stylesheetFilename = stylesheetFilename;
  this->stylesheetFlags = stylesheetFlags;

  bool prevErrs = !styleErrors.isEmpty();
  styleErrors.clear();
  for (auto db: databases){
    db->LoadStyle(stylesheetFilename+suffix, stylesheetFlags, styleErrors);
  }
  if (prevErrs || (!styleErrors.isEmpty())){
    qWarning()<<"Failed to load stylesheet"<<(stylesheetFilename+suffix);
    emit styleErrorsChanged();
  }
  InvalidateVisualCache();
  emit stylesheetFilenameChanged();
  emit Redraw();
}

bool DBThread::GetObjectDetails(DBInstanceRef db,
                                const osmscout::ObjectFileRef& object,
                                QString &typeName,
                                osmscout::GeoCoord& coordinates,
                                osmscout::GeoBox& bbox
                                )
{
    if (object.GetType()==osmscout::RefType::refNode) {
      osmscout::NodeRef node;

      if (!db->database->GetNodeByOffset(object.GetFileOffset(), node)) {
        return false;
      }
      typeName = QString::fromUtf8(node->GetType()->GetName().c_str());
      coordinates = node->GetCoords();
      bbox = osmscout::GeoBox::BoxByCenterAndRadius(coordinates, 2.0);
    }
    else if (object.GetType()==osmscout::RefType::refArea) {
      osmscout::AreaRef area;

      if (!db->database->GetAreaByOffset(object.GetFileOffset(), area)) {
        return false;
      }
      typeName = QString::fromUtf8(area->GetType()->GetName().c_str());
      area->GetCenter(coordinates);
      area->GetBoundingBox(bbox);
    }
    else if (object.GetType()==osmscout::RefType::refWay) {
      osmscout::WayRef way;
      if (!db->database->GetWayByOffset(object.GetFileOffset(), way)) {
        return false;
      }
      typeName = QString::fromUtf8(way->GetType()->GetName().c_str());
      way->GetCenter(coordinates);
      way->GetBoundingBox(bbox);
    }
    return true;
}

bool DBThread::BuildLocationEntry(const osmscout::ObjectFileRef& object,
                                  const QString title,
                                  DBInstanceRef db,
                                  std::map<osmscout::FileOffset,osmscout::AdminRegionRef> &/*adminRegionMap*/,
                                  QList<LocationEntry> &locations
                                  )
{
    QStringList adminRegionList;
    QString objectType;
    osmscout::GeoCoord coordinates;
    osmscout::GeoBox bbox;

    if (!GetObjectDetails(db, object, objectType, coordinates, bbox)){
      return false;
    }
    qDebug() << "obj:    " << title << "(" << objectType << ")";

    // This is very slow for some areas.
    /*
    std::list<osmscout::LocationService::ReverseLookupResult> result;
    if (db->locationService->ReverseLookupObject(object, result)){
        for (const osmscout::LocationService::ReverseLookupResult& entry : result){
            if (entry.adminRegion){
              adminRegionList = DBThread::BuildAdminRegionList(entry.adminRegion, adminRegionMap);
              break;
            }
        }
    }
     */

    LocationEntry location(LocationEntry::typeObject, title, objectType, adminRegionList,
                           db->path, coordinates, bbox);
    location.addReference(object);
    locations.append(location);

    return true;
}

bool DBThread::BuildLocationEntry(const osmscout::LocationSearchResult::Entry &entry,
                                  DBInstanceRef db,
                                  std::map<osmscout::FileOffset,osmscout::AdminRegionRef> &adminRegionMap,
                                  QList<LocationEntry> &locations
                                  )
{
    if (entry.adminRegion){
      db->locationService->ResolveAdminRegionHierachie(entry.adminRegion, adminRegionMap);
    }

    QStringList adminRegionList = DBThread::BuildAdminRegionList(entry.adminRegion, adminRegionMap);
    QString objectType;
    osmscout::GeoCoord coordinates;
    osmscout::GeoBox bbox;

    if (entry.adminRegion &&
        entry.location &&
        entry.address) {

      QString loc=QString::fromUtf8(entry.location->name.c_str());
      QString address=QString::fromUtf8(entry.address->name.c_str());

      QString label;
      label+=loc;
      label+=" ";
      label+=address;

      if (!GetObjectDetails(db, entry.address->object, objectType, coordinates, bbox)){
        return false;
      }

      qDebug() << "address:  " << label;

      LocationEntry location(LocationEntry::typeObject, label, objectType, adminRegionList,
                             db->path, coordinates, bbox);
      location.addReference(entry.address->object);
      locations.append(location);
    }
    else if (entry.adminRegion &&
             entry.location) {

      QString loc=QString::fromUtf8(entry.location->name.c_str());
      if (!GetObjectDetails(db, entry.location->objects.front(), objectType, coordinates, bbox)){
        return false;
      }

      qDebug() << "loc:      " << loc;

      LocationEntry location(LocationEntry::typeObject, loc, objectType, adminRegionList,
                             db->path, coordinates, bbox);

      for (std::vector<osmscout::ObjectFileRef>::const_iterator object=entry.location->objects.begin();
          object!=entry.location->objects.end();
          ++object) {
          location.addReference(*object);
      }
      locations.append(location);
    }
    else if (entry.adminRegion &&
             entry.poi) {

      QString poi=QString::fromUtf8(entry.poi->name.c_str());
      if (!GetObjectDetails(db, entry.poi->object, objectType, coordinates, bbox)){
        return false;
      }
      qDebug() << "poi:      " << poi;

      LocationEntry location(LocationEntry::typeObject, poi, objectType, adminRegionList,
                             db->path, coordinates, bbox);
      location.addReference(entry.poi->object);
      locations.append(location);
    }
    else if (entry.adminRegion) {
      QString objectType;
      if (entry.adminRegion->aliasObject.Valid()) {
          if (!GetObjectDetails(db, entry.adminRegion->aliasObject, objectType, coordinates, bbox)){
            return false;
          }
      }
      else {
          if (!GetObjectDetails(db, entry.adminRegion->object, objectType, coordinates, bbox)){
            return false;
          }
      }
      QString name;
      if (!entry.adminRegion->aliasName.empty()) {
        name=QString::fromUtf8(entry.adminRegion->aliasName.c_str());
      }
      else {
        name=QString::fromUtf8(entry.adminRegion->name.c_str());
      }

      //=QString::fromUtf8(entry.adminRegion->name.c_str());
      LocationEntry location(LocationEntry::typeObject, name, objectType, adminRegionList,
                             db->path, coordinates, bbox);

      qDebug() << "region: " << name;

      if (entry.adminRegion->aliasObject.Valid()) {
          location.addReference(entry.adminRegion->aliasObject);
      }
      else {
          location.addReference(entry.adminRegion->object);
      }
      locations.append(location);
    }
    return true;
}

void DBThread::SearchForLocations(const QString searchPattern, int limit)
{
  QReadLocker locker(&lock);

  qDebug() << "Searching for" << searchPattern;
  QTime timer;
  timer.start();

  std::string stdSearchPattern=searchPattern.toUtf8().constData();

  for (auto db:databases){
    std::map<osmscout::FileOffset,osmscout::AdminRegionRef> adminRegionMap;
    QList<LocationEntry> locations;

    // Search by location
    osmscout::LocationSearch search;
    search.limit=limit;
    osmscout::LocationSearchResult result;

    if (!db->locationService->InitializeLocationSearchEntries(stdSearchPattern, search)) {
      emit searchFinished(searchPattern, /*error*/ true);
      return;
    }

    if (!db->locationService->SearchForLocations(search, result)){
      emit searchFinished(searchPattern, /*error*/ true);
      return;
    }

    for (auto &entry: result.results){
      if (!BuildLocationEntry(entry, db, adminRegionMap, locations)){
        emit searchFinished(searchPattern, /*error*/ true);
        return;
      }
    }

    emit searchResult(searchPattern, locations);

#ifdef OSMSCOUT_HAVE_LIB_MARISA
    // Search by free text
    locations.clear();
    QList<osmscout::ObjectFileRef> objectSet;
    osmscout::TextSearchIndex textSearch;
    if(!textSearch.Load(db->path.toStdString())){
        qWarning("Failed to load text index files, search was for locations only");
        continue; // silently continue, text indexes are optional in database
    }
    osmscout::TextSearchIndex::ResultsMap resultsTxt;
    textSearch.Search(searchPattern.toStdString(),
                      /*searchPOIs*/ true, /*searchLocations*/ true,
                      /*searchRegions*/ true, /*searchOther*/ true,
                      resultsTxt);
    osmscout::TextSearchIndex::ResultsMap::iterator it;
    int count = 0;
    for(it=resultsTxt.begin();
      it != resultsTxt.end() && count < limit;
      ++it, ++count)
    {
        std::vector<osmscout::ObjectFileRef> &refs=it->second;

        std::size_t maxPrintedOffsets=5;
        std::size_t minRefCount=std::min(refs.size(),maxPrintedOffsets);

        for(size_t r=0; r < minRefCount; r++){
            if (locations.size()>=limit)
                break;

            osmscout::ObjectFileRef fref = refs[r];

            if (objectSet.contains(fref))
                continue;

            objectSet << fref;
            BuildLocationEntry(fref, QString::fromStdString(it->first),
                               db, adminRegionMap, locations);
        }
      }
      // TODO: merge locations with same label database type
      // bus stations can have more points for example...
      emit searchResult(searchPattern, locations);
#endif
  }

  qDebug() << "Retrieve result tooks" << timer.elapsed() << "ms";
  emit searchFinished(searchPattern, /*error*/ false);
}

bool DBThread::CalculateRoute(const QString databasePath,
                              const osmscout::RoutingProfile& routingProfile,
                              const osmscout::RoutePosition& start,
                              const osmscout::RoutePosition target,
                              osmscout::RouteData& route)
{
  QReadLocker locker(&lock);

  DBInstanceRef database;
  for (auto &db:databases){
    if (db->path==databasePath){
      database=db;
      break;
    }
  }
  if (!database){
    return false;
  }

  if (!database->AssureRouter(routingProfile.GetVehicle(), routerParameter)) {
    return false;
  }

  osmscout::RoutingResult    result;
  osmscout::RoutingParameter parameter;

  result=database->router->CalculateRoute(routingProfile,
                                          start,
                                          target,
                                          parameter);

  bool success=result.Success();

  route=std::move(result.GetRoute());

  return success;
}

bool DBThread::TransformRouteDataToRouteDescription(const QString databasePath,
                                                    const osmscout::RoutingProfile& routingProfile,
                                                    const osmscout::RouteData& data,
                                                    osmscout::RouteDescription& description,
                                                    const std::string& start,
                                                    const std::string& target)
{
  QReadLocker locker(&lock);

  DBInstanceRef database;
  for (auto &db:databases){
    if (db->path==databasePath){
      database=db;
      break;
    }
  }
  if (!database){
    return false;
  }

  if (!database->AssureRouter(routingProfile.GetVehicle(), routerParameter)) {
    return false;
  }

  if (!database->router->TransformRouteDataToRouteDescription(data,description)) {
    return false;
  }

  osmscout::TypeConfigRef typeConfig=database->router->GetTypeConfig();

  std::list<osmscout::RoutePostprocessor::PostprocessorRef> postprocessors;

  postprocessors.push_back(std::make_shared<osmscout::RoutePostprocessor::DistanceAndTimePostprocessor>());
  postprocessors.push_back(std::make_shared<osmscout::RoutePostprocessor::StartPostprocessor>(start));
  postprocessors.push_back(std::make_shared<osmscout::RoutePostprocessor::TargetPostprocessor>(target));
  postprocessors.push_back(std::make_shared<osmscout::RoutePostprocessor::WayNamePostprocessor>());
  postprocessors.push_back(std::make_shared<osmscout::RoutePostprocessor::CrossingWaysPostprocessor>());
  postprocessors.push_back(std::make_shared<osmscout::RoutePostprocessor::DirectionPostprocessor>());

  osmscout::RoutePostprocessor::InstructionPostprocessorRef instructionProcessor=std::make_shared<osmscout::RoutePostprocessor::InstructionPostprocessor>();

  instructionProcessor->AddMotorwayType(typeConfig->GetTypeInfo("highway_motorway"));
  instructionProcessor->AddMotorwayLinkType(typeConfig->GetTypeInfo("highway_motorway_link"));
  instructionProcessor->AddMotorwayType(typeConfig->GetTypeInfo("highway_motorway_trunk"));
  instructionProcessor->AddMotorwayType(typeConfig->GetTypeInfo("highway_trunk"));
  instructionProcessor->AddMotorwayLinkType(typeConfig->GetTypeInfo("highway_trunk_link"));
  instructionProcessor->AddMotorwayType(typeConfig->GetTypeInfo("highway_motorway_primary"));
  postprocessors.push_back(instructionProcessor);

  if (!routePostprocessor.PostprocessRouteDescription(description,
                                                      routingProfile,
                                                      *(database->database),
                                                      postprocessors)) {
    return false;
  }

  return true;
}

bool DBThread::TransformRouteDataToWay(const QString databasePath,
                                       osmscout::Vehicle vehicle,
                                       const osmscout::RouteData& data,
                                       osmscout::Way& way)
{
  QReadLocker locker(&lock);

  DBInstanceRef database;
  for (auto &db:databases){
    if (db->path==databasePath){
      database=db;
      break;
    }
  }
  if (!database){
    return false;
  }


  if (!database->AssureRouter(vehicle, routerParameter)) {
    return false;
  }

  return database->router->TransformRouteDataToWay(data,way);
}

void DBThread::ClearRoute()
{
  emit Redraw();
}

void DBThread::AddRoute(const osmscout::Way& /*way*/)
{
  emit Redraw();
}

osmscout::RoutePosition DBThread::GetClosestRoutableNode(const QString databasePath,
                                                         const osmscout::ObjectFileRef& refObject,
                                                         const osmscout::RoutingProfile& routingProfile,
                                                         double radius)
{
  QReadLocker locker(&lock);
  osmscout::RoutePosition position;

  DBInstanceRef database;
  for (auto &db:databases){
    if (db->path==databasePath){
      database=db;
      break;
    }
  }
  if (!database){
    return position;
  }

  if (!database->AssureRouter(routingProfile.GetVehicle(), routerParameter)) {
    return position;
  }

  if (refObject.GetType()==osmscout::refNode) {
    osmscout::NodeRef node;

    if (!database->database->GetNodeByOffset(refObject.GetFileOffset(), node)) {
      return position;
    }

    return database->router->GetClosestRoutableNode(node->GetCoords(),
                                                    routingProfile,
                                                    radius);
  }
  else if (refObject.GetType()==osmscout::refArea) {
    osmscout::AreaRef area;

    if (!database->database->GetAreaByOffset(refObject.GetFileOffset(), area)) {
      return position;
    }

    osmscout::GeoCoord center;

    area->GetCenter(center);

    return database->router->GetClosestRoutableNode(center,
                                                    routingProfile,
                                                    radius);
  }
  else if (refObject.GetType()==osmscout::refWay) {
    osmscout::WayRef way;

    if (!database->database->GetWayByOffset(refObject.GetFileOffset(), way)) {
      return position;
    }

    return database->router->GetClosestRoutableNode(way->nodes[0].GetCoord(),
                                                    routingProfile,
                                                    radius);
  }

  return position;
}

QStringList DBThread::BuildAdminRegionList(const osmscout::AdminRegionRef& adminRegion,
                                           std::map<osmscout::FileOffset,osmscout::AdminRegionRef> regionMap)
{
  return BuildAdminRegionList(osmscout::LocationServiceRef(), adminRegion, regionMap);
}

QStringList DBThread::BuildAdminRegionList(const osmscout::LocationServiceRef& locationService,
                                           const osmscout::AdminRegionRef& adminRegion,
                                           std::map<osmscout::FileOffset,osmscout::AdminRegionRef> regionMap)
{
  if (!adminRegion){
    return QStringList();
  }

  QStringList list;
  if (locationService)
    locationService->ResolveAdminRegionHierachie(adminRegion, regionMap);
  QString name = QString::fromStdString(adminRegion->name);
  list << name;
  QString last = name;
  osmscout::FileOffset parentOffset = adminRegion->parentRegionOffset;
  while (parentOffset != 0){
    const auto &it = regionMap.find(parentOffset);
    if (it==regionMap.end())
      break;
    const osmscout::AdminRegionRef region=it->second;
    name = QString::fromStdString(region->name);
    if (last != name){ // skip duplicates in admin region names
      list << name;
    }
    last = name;
    parentOffset = region->parentRegionOffset;
  }
  return list;
}

void DBThread::requestLocationDescription(const osmscout::GeoCoord location)
{
  QReadLocker locker(&lock);
  if (!isInitializedInternal()){
      return; // ignore request if db is not initialized
  }

  int count = 0;
  for (auto db:databases){
    osmscout::LocationDescription description;
    osmscout::GeoBox dbBox;
    if (!db->database->GetBoundingBox(dbBox)){
      continue;
    }
    if (!dbBox.Includes(location)){
      continue;
    }

    std::map<osmscout::FileOffset,osmscout::AdminRegionRef> regionMap;
    if (!db->locationService->DescribeLocationByAddress(location, description)) {
      std::cerr << "Error during generation of location description" << std::endl;
      continue;
    }

    if (description.GetAtAddressDescription()){
      count++;

      auto place = description.GetAtAddressDescription()->GetPlace();
      emit locationDescription(location, db->path, description,
                               BuildAdminRegionList(db->locationService, place.GetAdminRegion(), regionMap));
    }

    if (!db->locationService->DescribeLocationByPOI(location, description)) {
      std::cerr << "Error during generation of location description" << std::endl;
      continue;
    }

    if (description.GetAtPOIDescription()){
      count++;

      auto place = description.GetAtPOIDescription()->GetPlace();
      emit locationDescription(location, db->path, description,
                               BuildAdminRegionList(db->locationService, place.GetAdminRegion(), regionMap));
    }
  }

  emit locationDescriptionFinished(location);
}

void DBThread::onMapDPIChange(double dpi)
{
  {
    QWriteLocker locker(&lock);
    mapDpi = dpi;
  }
  InvalidateVisualCache();
  emit Redraw();
}

void DBThread::onRenderSeaChanged(bool b)
{
  {
    QWriteLocker locker(&lock);
    renderSea = b;
  }
  InvalidateVisualCache();
  emit Redraw();
}

void DBThread::onFontNameChanged(const QString fontName)
{
  {
    QWriteLocker locker(&lock);
    this->fontName=fontName;
  }
  InvalidateVisualCache();
  emit Redraw();
}

void DBThread::onFontSizeChanged(double fontSize)
{
  {
    QWriteLocker locker(&lock);
    this->fontSize=fontSize;
  }
  InvalidateVisualCache();
  emit Redraw();
}

osmscout::TypeConfigRef DBThread::GetTypeConfig(const QString databasePath) const
{
  QReadLocker locker(&lock);
  for (auto &db:databases){
    if (db->path == databasePath){
      return db->database->GetTypeConfig();
    }
  }
  return osmscout::TypeConfigRef();
}

const QMap<QString,bool> DBThread::GetStyleFlags() const
{
  QReadLocker locker(&lock);
  QMap<QString,bool> flags;
  // add flag overrides
  for (const auto& flag : stylesheetFlags){
    flags[QString::fromStdString(flag.first)]=flag.second;
  }
  // add flags defined by stylesheet
  for (auto &db:databases){
    for (const auto& flag : db->styleConfig->GetFlags()){
      if (!flags.contains(QString::fromStdString(flag.first))){
        flags[QString::fromStdString(flag.first)]=flag.second;
      }
    }
  }
  
  return flags;
}

void DBThread::RunJob(DBJob *job)
{
  QReadLocker *locker=new QReadLocker(&lock);
  job->Run(databases,locker);
}
