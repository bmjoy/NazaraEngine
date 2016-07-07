#include <Editor/EditorWindow.hpp>
#include <Nazara/Core/Bitset.hpp>
#include <Nazara/Core/PluginManager.hpp>
#include <Nazara/Utility.hpp>
#include <Editor/ModelWidget.hpp>
#include <QtWidgets>
#include <iostream>

EditorWindow::EditorWindow(QWidget* parent) :
QMainWindow(parent),
m_faceFilling(Nz::FaceFilling_Fill)
{
	/*m_mainLayout = new QGridLayout;
	m_mainLayout->addWidget(m_modelWidget,  0, 0, 3, 3);
	m_mainLayout->addWidget(m_materialsDock, 0, 3, 1, 1);
	m_mainLayout->addWidget(m_submeshesDock,  1, 3, 1, 1);
	m_mainLayout->addWidget(m_textEdit,     3, 0, 1, 1);

	mainWidget->setLayout(m_mainLayout);*/

	m_modelWidget = new ModelWidget;
	setCentralWidget(m_modelWidget);

	m_materialsDock = new QDockWidget("Materials", this);
	m_materialsDock->setSizePolicy(QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred));

	m_materialList = new QListWidget(m_materialsDock);
	m_materialList->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_materialsDock->setWidget(m_materialList);
	addDockWidget(Qt::RightDockWidgetArea, m_materialsDock);

	connect(m_materialList, &QListWidget::itemDoubleClicked, this, &EditorWindow::OnEditMaterial);

	m_submeshesDock = new QDockWidget("Submeshes", this);
	m_submeshesDock->setSizePolicy(QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred));

	m_subMeshList = new QListWidget(m_submeshesDock);
	m_subMeshList->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_submeshesDock->setWidget(m_subMeshList);
	addDockWidget(Qt::RightDockWidgetArea, m_submeshesDock);

	connect(m_subMeshList, &QListWidget::itemSelectionChanged, this, &EditorWindow::OnSubmeshChanged);

	m_consoleDock = new QDockWidget("Console", this);
	m_consoleDock->setSizePolicy(QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred));

	m_textEdit = new QTextEdit(m_consoleDock);
	m_textEdit->setReadOnly(true);

	m_consoleDock->setWidget(m_textEdit);
	addDockWidget(Qt::BottomDockWidgetArea, m_consoleDock);

	connect(m_subMeshList, &QListWidget::itemSelectionChanged, this, &EditorWindow::OnSubmeshChanged);

	m_materialEditor = new MaterialEditor(this);
	addDockWidget(Qt::LeftDockWidgetArea, m_materialEditor);
	m_materialEditor->setFloating(true);
	m_materialEditor->hide();

	BuildMenu();

	if (Nz::PluginManager::Mount(Nz::Plugin_Assimp))
		statusBar()->showMessage("Assimp plugin mounted");
	else
		statusBar()->showMessage("Failed to mount Assimp plugin");
}

EditorWindow::~EditorWindow()
{
}

void EditorWindow::BuildMenu()
{
	QMenu* menuFichier = menuBar()->addMenu("&Fichier");
	QAction* import = menuFichier->addAction("Importer...");
	connect(import, &QAction::triggered, this, &EditorWindow::OnImport);

	QMenu* fichiersRecents = menuFichier->addMenu("Fichiers &récents");
	fichiersRecents->addAction("Fichier bidon 1.txt");
	fichiersRecents->addAction("Fichier bidon 2.txt");
	fichiersRecents->addAction("Fichier bidon 3.txt");

	QMenu* menuEdition = menuBar()->addMenu("&Edition");
	QAction* flipUVs = menuEdition->addAction("Inverser les coordonnées de texture");
	connect(flipUVs, &QAction::triggered, this, &EditorWindow::OnFlipUVs);

	QMenu* drawMenu = menuBar()->addMenu("&Affichage");
	drawMenu->addSection("Editor");
	drawMenu->addAction(m_materialsDock->toggleViewAction());
	drawMenu->addAction(m_submeshesDock->toggleViewAction());
	drawMenu->addAction(m_consoleDock->toggleViewAction());
	drawMenu->addSection("Model");

	m_showNormalButton = new QAction("Draw normals", drawMenu);
	m_showNormalButton->setCheckable(true);
	drawMenu->addAction(m_showNormalButton);
	connect(m_showNormalButton, &QAction::toggled, this, &EditorWindow::OnNormalToggled);

	QMenu* fillModeMenu = drawMenu->addMenu("&Fill mode");

	QActionGroup* fillModeGroup = new QActionGroup(this);
	fillModeGroup->setExclusive(true);
	connect(fillModeGroup, &QActionGroup::triggered, [this] (QAction* action)
	{
		m_faceFilling = static_cast<Nz::FaceFilling>(action->data().toUInt());
		UpdateFaceFilling();
	});

	QAction* fillButton = fillModeGroup->addAction("Fill");
	fillButton->setCheckable(true);
	fillButton->setChecked(true);
	fillButton->setData(static_cast<unsigned int>(Nz::FaceFilling_Fill));
	fillModeMenu->addAction(fillButton);

	QAction* lineButton = fillModeGroup->addAction("Line");
	lineButton->setCheckable(true);
	lineButton->setData(static_cast<unsigned int>(Nz::FaceFilling_Line));
	fillModeMenu->addAction(lineButton);

	QAction* pointButton = fillModeGroup->addAction("Point");
	pointButton->setCheckable(true);
	pointButton->setData(static_cast<unsigned int>(Nz::FaceFilling_Point));
	fillModeMenu->addAction(pointButton);
}

void EditorWindow::SetModel(Nz::ModelRef model)
{
	m_model = std::move(model);
	m_modelWidget->OnModelChanged(m_model);

	Nz::Mesh* mesh = m_model->GetMesh();

	m_materialList->clear();
	m_materialList->addItem("All materials");

	std::size_t materialCount = mesh->GetMaterialCount();
	for (std::size_t i = 0; i < materialCount; ++i)
	{
		Nz::String name = "Material #" + Nz::String::Number(i);

		Nz::ParameterList matData = mesh->GetMaterialData(i);
		Nz::String matName;
		if (matData.GetStringParameter(Nz::MaterialData::Name, &matName))
			name += " - " + matName;

		QListWidgetItem* item = new QListWidgetItem(name.GetConstBuffer());
		item->setData(Qt::UserRole, i);

		m_materialList->addItem(item);
	}

	m_subMeshList->clear();
	m_subMeshList->addItem("All submeshes");

	std::size_t subMeshCount = mesh->GetSubMeshCount();
	for (std::size_t i = 0; i < subMeshCount; ++i)
	{
		Nz::String name = "Submesh #" + Nz::String::Number(i);

		QListWidgetItem* item = new QListWidgetItem(name.GetConstBuffer());
		item->setData(Qt::UserRole, i);

		m_subMeshList->addItem(item);
	}

	if (m_showNormalButton->isChecked())
		m_modelWidget->ShowNormals(true);

	OnSubmeshChanged();
	UpdateFaceFilling();
}

void EditorWindow::ShowSubmeshes(const Nz::Bitset<>& submeshes)
{
	m_activeSubmeshes = submeshes;

	m_modelWidget->ShowSubmeshes(submeshes);
}

void EditorWindow::OnEditMaterial(QListWidgetItem* item)
{
	QVariant data = item->data(Qt::UserRole);
	if (data.isNull())
		return;

	Nz::ParameterList parameters;
	m_model->GetMaterial(data.toUInt())->SaveToParameters(&parameters);

	m_materialEditedSlot.Connect(m_materialEditor->OnMaterialEditorSave, this, &EditorWindow::OnMaterialEdited);

	m_materialEditor->FillValues(data.toUInt(), parameters);
	m_materialEditor->show();
}

void EditorWindow::OnFlipUVs()
{
	if (m_model)
	{
		Nz::Mesh* mesh = m_model->GetMesh();
		std::size_t subMeshCount = mesh->GetSubMeshCount();

		for (std::size_t i = 0; i < subMeshCount; ++i)
		{
			if (!m_activeSubmeshes.Test(i))
				continue;

			Nz::SubMesh* submesh = mesh->GetSubMesh(i);
			Nz::VertexMapper mapper(submesh);
			std::size_t vertexCount = submesh->GetVertexCount();

			Nz::SparsePtr<Nz::Vector2f> texCoords = mapper.GetComponentPtr<Nz::Vector2f>(Nz::VertexComponent_TexCoord);

			for (unsigned int j = 0; j < vertexCount; ++j)
			{
				texCoords->Set(texCoords->x, 1.0f - texCoords->y);
				++texCoords;
			}
		}
	}
}

void EditorWindow::OnImport()
{
	QString filePath = QFileDialog::getOpenFileName(this, "Import a model");
	if (filePath.isEmpty())
		return;

	Nz::ErrorFlags errFlags(Nz::ErrorFlag_Silent | Nz::ErrorFlag_ThrowException, true);

	Nz::ModelRef model = Nz::Model::New();

	Nz::Clock loadClock;

	try
	{
		Nz::ModelParameters parameters;
		parameters.mesh.optimizeIndexBuffers = false;

		model->LoadFromFile(filePath.toUtf8().constData(), parameters);
	}
	catch (const std::exception& e)
	{
		Nz::String message = "Failed to load model: ";
		message += e.what();

		m_textEdit->append(message.GetConstBuffer());

		return;
	}

	SetModel(std::move(model));

	statusBar()->showMessage("Loaded " + filePath + " in " + QString::number(loadClock.GetSeconds()) + "s");
}

void EditorWindow::OnMaterialEdited(MaterialEditor* editor, std::size_t matIndex, const Nz::ParameterList& materialParameters)
{
	Nz::Material* mat = m_model->GetMaterial(matIndex);
	mat->Reset();
	mat->BuildFromParameters(materialParameters);
}

void EditorWindow::OnNormalToggled(bool active)
{
	m_modelWidget->ShowNormals(active);
}

void EditorWindow::OnSubmeshChanged()
{
	auto selectedItems = m_subMeshList->selectedItems();

	Nz::Bitset<> activeSubmeshes(m_model->GetMesh()->GetSubMeshCount(), false);
	if (selectedItems.isEmpty())
		activeSubmeshes.Set(true);
	else
	{
		for (QListWidgetItem* item : selectedItems)
		{
			QVariant data = item->data(Qt::UserRole);
			if (data.isNull())
				activeSubmeshes.Set(true);
			else
				activeSubmeshes.Set(data.toInt(), true);
		}
	}

	ShowSubmeshes(activeSubmeshes);
}

void EditorWindow::UpdateFaceFilling()
{
	Nz::Mesh* mesh = m_model->GetMesh();

	std::size_t materialCount = mesh->GetMaterialCount();
	for (std::size_t i = 0; i < materialCount; ++i)
		m_model->GetMaterial(i)->SetFaceFilling(m_faceFilling);
}

#include <Editor/EditorWindow_moc.inl>