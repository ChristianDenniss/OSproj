fetch("/data.json")
    .then((response) => response.json())
    .then((data) =>
    {
        document.getElementById("info").textContent = JSON.stringify(data, null, 2);
    })
    .catch((error) =>
    {
        document.getElementById("info").textContent = "Failed to load data.json: " + error;
    });
